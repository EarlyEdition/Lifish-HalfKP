/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#include <iostream>
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "timeman.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

namespace Search {

  LimitsType Limits;
}


using std::string;
using Eval::evaluate;
using namespace Search;

namespace {

  // Different node types, used as a template parameter
  enum NodeType { NonPV, PV };

  // Sizes and phases of the skip-blocks, used for distributing search depths across the threads
  const int skipSize[]  = { 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4 };
  const int skipPhase[] = { 0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7 };

  // Razoring and futility margin based on depth
  const int razor_margin = 600;
//Value futility_margin(Depth d) { return Value(150 * d / ONE_PLY); }
  Value futility_margin(Depth d) { return Value(150 * d / ONE_PLY); }

  // Futility and reductions lookup tables, initialized at startup
  int FutilityMoveCounts[2][16]; // [improving][depth]
  int Reductions[2][2][64][64];  // [pv][improving][depth][moveNumber]

  template <bool PvNode> Depth reduction(bool i, Depth d, int mn) {
    return Reductions[PvNode][i][std::min(d / ONE_PLY, 63)][std::min(mn, 63)] * ONE_PLY;
  }

  // History and stats update bonus, based on depth
  int stat_bonus(Depth depth) {
    int d = depth / ONE_PLY;
    return d > 17 ? 0 : d * d + 2 * d - 2;
  }


  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode, bool skipEarlyPruning);

  template <NodeType NT, bool InCheck>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = DEPTH_ZERO);

  Value value_to_tt(Value v, int ply);
  Value value_from_tt(Value v, int ply);
  void update_pv(Move* pv, Move move, Move* childPv);
  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
  void update_stats(const Position& pos, Stack* ss, Move move, Move* quiets, int quietsCnt, int bonus);
  void update_capture_stats(const Position& pos, Move move, Move* captures, int captureCnt, int bonus);
  bool pv_is_draw(Position& pos);

  // perft() is our utility to verify move generation. All the leaf nodes up
  // to the given depth are generated and counted, and the sum is returned.
  template<bool Root>
  uint64_t perft(Position& pos, Depth depth) {

    StateInfo st;
    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2 * ONE_PLY);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (Root && depth <= ONE_PLY)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - ONE_PLY);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << UCI::move(m, pos.is_chess960()) << ": " << cnt << sync_endl;
    }
    return nodes;
  }

} // namespace


/// Search::init() is called during startup to initialize various lookup tables

void Search::init() {

  for (int imp = 0; imp <= 1; ++imp)
      for (int d = 1; d < 64; ++d)
          for (int mc = 1; mc < 64; ++mc)
          {
              double r = log(d) * log(mc) / 1.95;

              Reductions[NonPV][imp][d][mc] = int(std::round(r));
              Reductions[PV][imp][d][mc] = std::max(Reductions[NonPV][imp][d][mc] - 1, 0);

              // Increase reduction for non-PV nodes when eval is not improving
              if (!imp && Reductions[NonPV][imp][d][mc] >= 2)
                Reductions[NonPV][imp][d][mc]++;
          }

  for (int d = 0; d < 16; ++d)
  {
      FutilityMoveCounts[0][d] = int(2.4 + 0.74 * pow(d, 1.78));
      FutilityMoveCounts[1][d] = int(5.0 + 1.00 * pow(d, 2.00));
  }
}


/// Search::clear() resets search state to its initial value

void Search::clear() {

  Threads.main()->wait_for_search_finished();

  Time.availableNodes = 0;
  TT.clear();
  Threads.clear();
}


/// MainThread::search() is called by the main thread when the program receives
/// the UCI 'go' command. It searches from the root position and outputs the "bestmove".

void MainThread::search() {

  if (Limits.perft)
  {
      nodes = perft<true>(rootPos, Limits.perft * ONE_PLY);
      sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
      return;
  }

  Color us = rootPos.side_to_move();
  Time.init(Limits, us, rootPos.game_ply());
  TT.new_search();

  Eval::NNUE::verify();

  if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      sync_cout << "info depth 0 score "
                << UCI::value(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW)
                << sync_endl;
  }
  else
  {
      for (Thread* th : Threads)
          if (th != this)
              th->start_searching();

      Thread::search(); // Let's start searching!
  }

  // When we reach the maximum depth, we can arrive here without a raise of
  // Threads.stop. However, if we are pondering or in an infinite search,
  // the UCI protocol states that we shouldn't print the best move before the
  // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
  // until the GUI sends one of those commands (which also raises Threads.stop).
  Threads.stopOnPonderhit = true;

  while (!Threads.stop && (Threads.ponder || Limits.infinite))
  {} // Busy wait for a stop or a ponder reset

  // Stop the threads if not already stopped (also raise the stop if
  // "ponderhit" just reset Threads.ponder).
  Threads.stop = true;

  // Wait until all threads have finished
  for (Thread* th : Threads)
      if (th != this)
          th->wait_for_search_finished();

  // When playing in 'nodes as time' mode, subtract the searched nodes from
  // the available ones before exiting.
  if (Limits.npmsec)
      Time.availableNodes += Limits.inc[us] - Threads.nodes_searched();

  // Check if there are threads with a better score than main thread
  Thread* bestThread = this;
  if (    Options["MultiPV"] == 1 &&  rootMoves[0].pv[0] != MOVE_NONE)
  {
      for (Thread* th : Threads)
      {
          Depth depthDiff = th->completedDepth - bestThread->completedDepth;
          Value scoreDiff = th->rootMoves[0].score - bestThread->rootMoves[0].score;

          // Select the thread with the best score, always if it is a mate
          if (    scoreDiff > 0
              && (depthDiff >= 0 || th->rootMoves[0].score >= VALUE_MATE_IN_MAX_PLY))
              bestThread = th;
      }
  }

  previousScore = bestThread->rootMoves[0].score;

  // Send new PV when needed
  if (bestThread != this)
      sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;

  sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());

  if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
      std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

  std::cout << sync_endl;
}


/// Thread::search() is the main iterative deepening loop. It calls search()
/// repeatedly with increasing depth until the allocated thinking time has been
/// consumed, the user stops the search, or the maximum search depth is reached.

void Thread::search() {

  Stack stack[MAX_PLY+7], *ss = stack+4; // To reference from (ss-4) to (ss+2)
  Value bestValue, alpha, beta, delta;
  Move  lastBestMove = MOVE_NONE;
  Depth lastBestMoveDepth = DEPTH_ZERO;
  MainThread* mainThread = (this == Threads.main() ? Threads.main() : nullptr);
  double timeReduction = 1.0;

  std::memset(ss-4, 0, 7 * sizeof(Stack));
  for (int i = 4; i > 0; i--)
     (ss-i)->contHistory = &this->contHistory[NO_PIECE][0]; // Use as sentinel

  bestValue = delta = alpha = -VALUE_INFINITE;
  beta = VALUE_INFINITE;

  if (mainThread)
  {
      mainThread->failedLow = false;
      mainThread->bestMoveChanges = 0;
  }

  size_t multiPV = Options["MultiPV"];

  multiPV = std::min(multiPV, rootMoves.size());

  // Iterative deepening loop until requested to stop or the target depth is reached
  while (   (rootDepth += ONE_PLY) < DEPTH_MAX
         && !Threads.stop
         && !(Limits.depth && mainThread && rootDepth / ONE_PLY > Limits.depth))
  {
      // Distribute search depths across the threads
      if (idx)
      {
          int i = (idx - 1) % 20;
          if (((rootDepth / ONE_PLY + rootPos.game_ply() + skipPhase[i]) / skipSize[i]) % 2)
              continue;
      }

      // Age out PV variability metric
      if (mainThread)
          mainThread->bestMoveChanges *= 0.505, mainThread->failedLow = false;

      // Save the last iteration's scores before first PV line is searched and
      // all the move scores except the (new) PV are set to -VALUE_INFINITE.
      for (RootMove& rm : rootMoves)
          rm.previousScore = rm.score;

      // MultiPV loop. We perform a full root search for each PV line
      for (PVIdx = 0; PVIdx < multiPV && !Threads.stop; ++PVIdx)
      {
          // Reset UCI info selDepth for each depth and each PV line
          selDepth = 0;

          // Reset aspiration window starting size
          if (rootDepth >= 5 * ONE_PLY)
          {
              delta = Value(18);
              alpha = std::max(rootMoves[PVIdx].previousScore - delta,-VALUE_INFINITE);
              beta  = std::min(rootMoves[PVIdx].previousScore + delta, VALUE_INFINITE);
          }

          // Start with a small aspiration window and, in the case of a fail
          // high/low, re-search with a bigger window until we're not failing
          // high/low anymore.
          while (true)
          {
              bestValue = ::search<PV>(rootPos, ss, alpha, beta, rootDepth, false, false);

              // Bring the best move to the front. It is critical that sorting
              // is done with a stable algorithm because all the values but the
              // first and eventually the new best one are set to -VALUE_INFINITE
              // and we want to keep the same order for all the moves except the
              // new PV that goes to the front. Note that in case of MultiPV
              // search the already searched PV lines are preserved.
              std::stable_sort(rootMoves.begin() + PVIdx, rootMoves.end());

              // If search has been stopped, we break immediately. Sorting and
              // writing PV back to TT is safe because RootMoves is still
              // valid, although it refers to the previous iteration.
              if (Threads.stop)
                  break;

              // When failing high/low give some update (without cluttering
              // the UI) before a re-search.
              if (   mainThread
                  && multiPV == 1
                  && (bestValue <= alpha || bestValue >= beta)
                  && Time.elapsed() > 3000)
                  sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;

              // In case of failing low/high increase aspiration window and
              // re-search, otherwise exit the loop.
              if (bestValue <= alpha)
              {
                  beta = (alpha + beta) / 2;
                  alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                  if (mainThread)
                  {
                      mainThread->failedLow = true;
                      Threads.stopOnPonderhit = false;
                  }
              }
              else if (bestValue >= beta)
                  beta = std::min(bestValue + delta, VALUE_INFINITE);
              else
                  break;

              delta += delta / 4 + 5;

              assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
          }

          // Sort the PV lines searched so far and update the GUI
          std::stable_sort(rootMoves.begin(), rootMoves.begin() + PVIdx + 1);

          if (    mainThread
              && (Threads.stop || PVIdx + 1 == multiPV || Time.elapsed() > 3000))
              sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;
      }

      if (!Threads.stop)
          completedDepth = rootDepth;

      if (rootMoves[0].pv[0] != lastBestMove) {
         lastBestMove = rootMoves[0].pv[0];
         lastBestMoveDepth = rootDepth;
      }

      // Have we found a "mate in x"?
      if (   Limits.mate
          && bestValue >= VALUE_MATE_IN_MAX_PLY
          && VALUE_MATE - bestValue <= 2 * Limits.mate)
          Threads.stop = true;

      if (!mainThread)
          continue;

      // Do we have time for the next iteration? Can we stop searching now?
      if (Limits.use_time_management())
      {
          if (!Threads.stop && !Threads.stopOnPonderhit)
          {
              // Stop the search if only one legal move is available, or if all
              // of the available time has been used
              const int F[] = { mainThread->failedLow,
                                bestValue - mainThread->previousScore };
              int improvingFactor = std::max(229, std::min(715, 357 + 119 * F[0] - 6 * F[1]));

              Color us = rootPos.side_to_move();
              bool thinkHard =    bestValue == VALUE_DRAW
                               && Limits.time[us] - Time.elapsed() > Limits.time[~us]
                               && ::pv_is_draw(rootPos);

              double unstablePvFactor = 1 + mainThread->bestMoveChanges + thinkHard;

              // if the bestMove is stable over several iterations, reduce time for this move,
              // the longer the move has been stable, the more.
              // Use part of the gained time from a previous stable move for the current move.
              timeReduction = 1;
              for (int i : {3, 4, 5})
                  if (lastBestMoveDepth * i < completedDepth && !thinkHard)
                     timeReduction *= 1.3;
              unstablePvFactor *=  std::pow(mainThread->previousTimeReduction, 0.51) / timeReduction;

              if (   rootMoves.size() == 1
                  || Time.elapsed() > Time.optimum() * unstablePvFactor * improvingFactor / 628)
              {
                  // If we are allowed to ponder do not stop the search now but
                  // keep pondering until the GUI sends "ponderhit" or "stop".
                  if (Threads.ponder)
                      Threads.stopOnPonderhit = true;
                  else
                      Threads.stop = true;
              }
          }
      }
  }

  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;

}


namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode, bool skipEarlyPruning) {

    const bool PvNode = NT == PV;
    const bool rootNode = PvNode && ss->ply == 0;

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(DEPTH_ZERO < depth && depth < DEPTH_MAX);
    assert(!(PvNode && cutNode));
    assert(depth / ONE_PLY * ONE_PLY == depth);

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
    TTEntry* tte;
    Key posKey;
    Move ttMove, move, excludedMove, bestMove;
    Depth extension, newDepth;
    Value bestValue, value, ttValue, eval, maxValue;
    bool ttHit, inCheck, givesCheck, singularExtensionNode, improving;
    bool captureOrPromotion, doFullDepthSearch, moveCountPruning, skipQuiets, ttCapture, pvExact;
    Piece movedPiece;
    int moveCount, captureCount, quietCount;

    // Step 1. Initialize node
    Thread* thisThread = pos.this_thread();
    inCheck = pos.checkers();
    moveCount = captureCount = quietCount = ss->moveCount = 0;
    ss->statScore = 0;
    bestValue = -VALUE_INFINITE;
    maxValue = VALUE_INFINITE;

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    if (!rootNode)
    {
        // Step 2. Check for aborted search and immediate draw
        if (Threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
            return ss->ply >= MAX_PLY && !inCheck ? evaluate(pos) : VALUE_DRAW;

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply+1), beta);
        if (alpha >= beta)
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->ply = ss->ply + 1;
    ss->currentMove = (ss+1)->excludedMove = bestMove = MOVE_NONE;
    ss->contHistory = &thisThread->contHistory[NO_PIECE][0];
    (ss+2)->killers[0] = (ss+2)->killers[1] = MOVE_NONE;
    Square prevSq = to_sq((ss-1)->currentMove);

    // Step 4. Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = pos.key() ^ Key(excludedMove << 16); // isn't a very good hash
    tte = TT.probe(posKey, ttHit);
    ttValue = ttHit ? value_from_tt(tte->value(), ss->ply) : VALUE_NONE;
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->PVIdx].pv[0]
            : ttHit    ? tte->move() : MOVE_NONE;

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ttHit
        && tte->depth() >= depth
        && ttValue != VALUE_NONE // Possible in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                if (!pos.capture_or_promotion(ttMove))
                    update_stats(pos, ss, ttMove, nullptr, 0, stat_bonus(depth));

                // Extra penalty for a quiet TT move in previous ply when it gets refuted
                if ((ss-1)->moveCount == 1 && !pos.captured_piece())
                    update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + ONE_PLY));
            }
            // Penalty for a quiet ttMove that fails low
            else if (!pos.capture_or_promotion(ttMove))
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory.update(pos.side_to_move(), ttMove, penalty);
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }
        return ttValue;
    }

    // Step 5. Evaluate the position statically
    if (inCheck)
    {
        ss->staticEval = eval = VALUE_NONE;
		improving = true;
        goto moves_loop;
    }

    else if (ttHit)
    {
        // Never assume anything on values stored in TT
        if ((ss->staticEval = eval = tte->eval()) == VALUE_NONE)
            eval = ss->staticEval = evaluate(pos);

        // Can ttValue be used as a better position evaluation?
        if (   ttValue != VALUE_NONE
            && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {
        eval = ss->staticEval =
        (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                         : -(ss-1)->staticEval + 2 * Eval::Tempo;

        tte->save(posKey, VALUE_NONE, BOUND_NONE, DEPTH_NONE, MOVE_NONE,
                  ss->staticEval, TT.generation());
    }

    if (skipEarlyPruning || !pos.non_pawn_material(pos.side_to_move()))
        goto moves_loop;

    // Step 6. Razoring (skipped when in check)
    if (   !PvNode
        &&  depth < 4 * ONE_PLY
        &&  eval + razor_margin <= alpha)
    {
        if (depth <= ONE_PLY)
            return qsearch<NonPV, false>(pos, ss, alpha, alpha+1);

        Value ralpha = alpha - razor_margin;
        Value v = qsearch<NonPV, false>(pos, ss, ralpha, ralpha+1);
        if (v <= ralpha)
            return v;
    }

    // Step 7. Futility pruning: child node (skipped when in check)
    if (   !rootNode
        &&  depth < 7 * ONE_PLY
        &&  eval - futility_margin(depth) >= beta
        &&  eval < VALUE_KNOWN_WIN)  // Do not return unproven wins
        return eval;

    // Step 8. Null move search with verification search (is omitted in PV nodes)
    if (   !PvNode
        &&  eval >= beta
        &&  ss->staticEval >= beta - 36 * depth / ONE_PLY + 225
        && (ss->ply >= thisThread->nmp_ply || ss->ply % 2 != thisThread->nmp_odd))
    {

        assert(eval - beta >= 0);

        // Null move dynamic reduction based on depth and value
        Depth R = ((823 + 67 * depth / ONE_PLY) / 256 + std::min((eval - beta) / PawnValueMg, 3)) * ONE_PLY;

        ss->currentMove = MOVE_NULL;
        ss->contHistory = &thisThread->contHistory[NO_PIECE][0];

        pos.do_null_move(st);
        Value nullValue = depth-R < ONE_PLY ? -qsearch<NonPV, false>(pos, ss+1, -beta, -beta+1)
                                            : - search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode, true);
        pos.undo_null_move();

        if (nullValue >= beta)
        {
            // Do not return unproven mate scores
            if (nullValue >= VALUE_MATE_IN_MAX_PLY)
                nullValue = beta;

            if (abs(beta) < VALUE_KNOWN_WIN && (depth < 12 * ONE_PLY || thisThread->nmp_ply))
                return nullValue;

            // Do verification search at high depths
            // disable null move pruning for side to move for the first part of the remaining search tree
            thisThread->nmp_ply = ss->ply + 3 * (depth-R) / 4;
            thisThread->nmp_odd = ss->ply % 2;

            Value v = depth-R < ONE_PLY ? qsearch<NonPV, false>(pos, ss, beta-1, beta)
                                        :  search<NonPV>(pos, ss, beta-1, beta, depth-R, false, true);

            thisThread->nmp_odd = thisThread->nmp_ply = 0;

            if (v >= beta)
                return nullValue;
        }
    }

    // Step 9. ProbCut (skipped when in check)
    // If we have a good enough capture and a reduced search returns a value
    // much above beta, we can (almost) safely prune the previous move.
    if (   !PvNode
        &&  depth >= 5 * ONE_PLY
        &&  abs(beta) < VALUE_MATE_IN_MAX_PLY)
    {
		Value rbeta = std::min(beta + 200, VALUE_INFINITE);

        assert(is_ok((ss-1)->currentMove));

        MovePicker mp(pos, ttMove, rbeta - ss->staticEval, &thisThread->captureHistory);

        while ((move = mp.next_move()) != MOVE_NONE)
            if (pos.legal(move))
            {
                ss->currentMove = move;
                ss->contHistory = &thisThread->contHistory[pos.moved_piece(move)][to_sq(move)];

                assert(depth >= 5 * ONE_PLY);
                pos.do_move(move, st);
                value = -search<NonPV>(pos, ss+1, -rbeta, -rbeta+1, depth - 4 * ONE_PLY, !cutNode, false);
                pos.undo_move(move);
                if (value >= rbeta)
                    return value;
            }
    }

    // Step 10. Internal iterative deepening (skipped when in check)
    if (    depth >= 6 * ONE_PLY
        && !ttMove
        && (PvNode || ss->staticEval + 256 >= beta))
    {
        Depth d = (3 * depth / (4 * ONE_PLY) - 2) * ONE_PLY;
        search<NT>(pos, ss, alpha, beta, d, cutNode, true);

        tte = TT.probe(posKey, ttHit);
        ttMove = ttHit ? tte->move() : MOVE_NONE;
    }

moves_loop: // When in check search starts from here

    const PieceToHistory* contHist[] = { (ss-1)->contHistory, (ss-2)->contHistory, nullptr, (ss-4)->contHistory };
    Move countermove = thisThread->counterMoves[pos.piece_on(prevSq)][prevSq];

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &thisThread->captureHistory, contHist, countermove, ss->killers);
    value = bestValue; // Workaround a bogus 'uninitialized' warning under gcc
    improving =   ss->staticEval >= (ss-2)->staticEval
            /* || ss->staticEval == VALUE_NONE Already implicit in the previous condition */
               ||(ss-2)->staticEval == VALUE_NONE;

    singularExtensionNode =   !rootNode
                           &&  depth >= 8 * ONE_PLY
                           &&  ttMove != MOVE_NONE
                           &&  ttValue != VALUE_NONE
                           && !excludedMove // Recursive singular search is not allowed
                           && (tte->bound() & BOUND_LOWER)
                           &&  tte->depth() >= depth - 3 * ONE_PLY;
    skipQuiets = false;
    ttCapture = false;
    pvExact = PvNode && ttHit && tte->bound() == BOUND_EXACT;

    // Step 11. Loop through moves
    // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move(skipQuiets)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->PVIdx,
                                  thisThread->rootMoves.end(), move))
          continue;

      ss->moveCount = ++moveCount;

      if (rootNode && thisThread == Threads.main() && Time.elapsed() > 3000)
          sync_cout << "info depth " << depth / ONE_PLY
                    << " currmove " << UCI::move(move, pos.is_chess960())
                    << " currmovenumber " << moveCount + thisThread->PVIdx << sync_endl;

      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = DEPTH_ZERO;
      captureOrPromotion = pos.capture_or_promotion(move);
      movedPiece = pos.moved_piece(move);

      givesCheck =  type_of(move) == NORMAL && !pos.discovered_check_candidates()
                  ? pos.check_squares(type_of(movedPiece)) & to_sq(move)
                  : pos.gives_check(move);

      moveCountPruning =   depth < 16 * ONE_PLY
                        && moveCount >= FutilityMoveCounts[improving][depth / ONE_PLY];

      // Step 12. Singular and Gives Check Extensions

      // Singular extension search. If all moves but one fail low on a search of
      // (alpha-s, beta-s), and just one fails high on (alpha, beta), then that move
      // is singular and should be extended. To verify this we do a reduced search
      // on all the other moves but the ttMove and if the result is lower than
      // ttValue minus a margin then we will extend the ttMove.
      if (    singularExtensionNode
          &&  move == ttMove
          &&  pos.legal(move))
      {
          Value rBeta = std::max(ttValue - 2 * depth / ONE_PLY, -VALUE_MATE);
          Depth d = (depth / (2 * ONE_PLY)) * ONE_PLY;
          ss->excludedMove = move;
          value = search<NonPV>(pos, ss, rBeta - 1, rBeta, d, cutNode, true);
          ss->excludedMove = MOVE_NONE;

          if (value < rBeta)
              extension = ONE_PLY;
      }
      else if (    givesCheck
               && !moveCountPruning
               &&  pos.see_ge(move))
          extension = ONE_PLY;

      // Calculate new depth for this move
      newDepth = depth - ONE_PLY + extension;

      // Step 13. Pruning at shallow depth
      if (  !rootNode
          && pos.non_pawn_material(pos.side_to_move())
          && bestValue > VALUE_MATED_IN_MAX_PLY)
      {
          if (   !captureOrPromotion
              && !givesCheck
              && (!pos.advanced_pawn_push(move) || pos.non_pawn_material() >= Value(5000)))
          {
              // Move count based pruning
              if (moveCountPruning)
              {
                  skipQuiets = true;
                  continue;
              }

              // Reduced depth of the next LMR search
              int lmrDepth = std::max(newDepth - reduction<PvNode>(improving, depth, moveCount), DEPTH_ZERO) / ONE_PLY;

              // Countermoves based pruning
              if (   lmrDepth < 3
                  && (*contHist[0])[movedPiece][to_sq(move)] < CounterMovePruneThreshold
                  && (*contHist[1])[movedPiece][to_sq(move)] < CounterMovePruneThreshold)
                  continue;

              // Futility pruning: parent node
              if (   lmrDepth < 7
                  && !inCheck
                  && ss->staticEval + 256 + 200 * lmrDepth <= alpha)
                  continue;

              // Prune moves with negative SEE
              if (   lmrDepth < 8
                  && !pos.see_ge(move, Value(-35 * lmrDepth * lmrDepth)))
                  continue;
          }
          else if (    depth < 7 * ONE_PLY
                   && !extension
                   && !pos.see_ge(move, -PawnValueEg * (depth / ONE_PLY)))
                  continue;
      }

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Check for legality just before making the move
      if (!rootNode && !pos.legal(move))
      {
          ss->moveCount = --moveCount;
          continue;
      }

      if (move == ttMove && captureOrPromotion)
          ttCapture = true;

      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->contHistory = &thisThread->contHistory[movedPiece][to_sq(move)];

      // Step 14. Make the move
      pos.do_move(move, st, givesCheck);

      // Step 15. Reduced depth search (LMR). If the move fails high it will be
      // re-searched at full depth.
      if (    depth >= 3 * ONE_PLY
          &&  moveCount > 1
          && (!captureOrPromotion || moveCountPruning))
      {
          Depth r = reduction<PvNode>(improving, depth, moveCount);

          if (captureOrPromotion)
              r -= r ? ONE_PLY : DEPTH_ZERO;
          else
          {
              // Decrease reduction if opponent's move count is high
              if ((ss-1)->moveCount > 15)
                  r -= ONE_PLY;

              // Decrease reduction for exact PV nodes
              if (pvExact)
                  r -= ONE_PLY;

              // Increase reduction if ttMove is a capture
              if (ttCapture)
                  r += ONE_PLY;

              // Increase reduction for cut nodes
              if (cutNode)
                  r += 2 * ONE_PLY;

              // Decrease reduction for moves that escape a capture. Filter out
              // castling moves, because they are coded as "king captures rook" and
              // hence break make_move().
              else if (    type_of(move) == NORMAL
                       && !pos.see_ge(make_move(to_sq(move), from_sq(move))))
                  r -= 2 * ONE_PLY;

              ss->statScore =  thisThread->mainHistory[~pos.side_to_move()][from_to(move)]
                             + (*contHist[0])[movedPiece][to_sq(move)]
                             + (*contHist[1])[movedPiece][to_sq(move)]
                             + (*contHist[3])[movedPiece][to_sq(move)]
                             - 4000;

              // Decrease/increase reduction by comparing opponent's stat score
              if (ss->statScore >= 0 && (ss-1)->statScore < 0)
                  r -= ONE_PLY;

              else if ((ss-1)->statScore >= 0 && ss->statScore < 0)
                  r += ONE_PLY;

              // Decrease/increase reduction for moves with a good/bad history
              r = std::max(DEPTH_ZERO, (r / ONE_PLY - ss->statScore / 20000) * ONE_PLY);
          }

          Depth d = std::max(newDepth - r, ONE_PLY);

          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true, false);

          doFullDepthSearch = (value > alpha && d != newDepth);
      }
      else
          doFullDepthSearch = !PvNode || moveCount > 1;

      // Step 16. Full depth search when LMR is skipped or fails high
      if (doFullDepthSearch)
          value = newDepth <   ONE_PLY ?
                            givesCheck ? -qsearch<NonPV,  true>(pos, ss+1, -(alpha+1), -alpha)
                                       : -qsearch<NonPV, false>(pos, ss+1, -(alpha+1), -alpha)
                                       : - search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode, false);

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;

          value = newDepth <   ONE_PLY ?
                            givesCheck ? -qsearch<PV,  true>(pos, ss+1, -beta, -alpha)
                                       : -qsearch<PV, false>(pos, ss+1, -beta, -alpha)
                                       : - search<PV>(pos, ss+1, -beta, -alpha, newDepth, false, false);
      }

      // Step 17. Undo move
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 18. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
          return VALUE_ZERO;

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);

          // PV move or new best move ?
          if (moveCount == 1 || value > alpha)
          {
              rm.score = value;
              rm.selDepth = thisThread->selDepth;
              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each
              // iteration. This information is used for time management: When
              // the best move changes frequently, we allocate some more time.
              if (moveCount > 1 && thisThread == Threads.main())
                  ++static_cast<MainThread*>(thisThread)->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
              // is not a problem when sorting because the sort is stable and the
              // move position in the list is preserved - just the PV is pushed up.
              rm.score = -VALUE_INFINITE;
      }

      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha! Always alpha < beta
                  alpha = value;
              else
              {
                  assert(value >= beta); // Fail high
                  break;
              }
          }
      }

      if (!captureOrPromotion && move != bestMove && quietCount < 64)
          quietsSearched[quietCount++] = move;
      else if (captureOrPromotion && move != bestMove && captureCount < 32)
          capturesSearched[captureCount++] = move;
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 20. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (!moveCount)
        bestValue = excludedMove ? alpha
                   :     inCheck ? mated_in(ss->ply) : VALUE_DRAW;
    else if (bestMove)
    {
        // Quiet best move: update move sorting heuristics
        if (!pos.capture_or_promotion(bestMove))
            update_stats(pos, ss, bestMove, quietsSearched, quietCount, stat_bonus(depth));
        else
            update_capture_stats(pos, bestMove, capturesSearched, captureCount, stat_bonus(depth));

        // Extra penalty for a quiet TT move in previous ply when it gets refuted
        if ((ss-1)->moveCount == 1 && !pos.captured_piece())
            update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + ONE_PLY));
    }
    // Bonus for prior countermove that caused the fail low
    else if (    depth >= 3 * ONE_PLY
             && !pos.captured_piece()
             && is_ok((ss-1)->currentMove))
        update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, stat_bonus(depth));

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    if (!excludedMove)
        tte->save(posKey, value_to_tt(bestValue, ss->ply),
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, ss->staticEval, TT.generation());

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main
  // search function with depth zero, or recursively with depth less than ONE_PLY.

  template <NodeType NT, bool InCheck>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    const bool PvNode = NT == PV;

    assert(InCheck == bool(pos.checkers()));
    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= DEPTH_ZERO);
    assert(depth / ONE_PLY * ONE_PLY == depth);

    Move pv[MAX_PLY+1];
    StateInfo st;
    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Value bestValue, value, ttValue, futilityValue, futilityBase, oldAlpha;
    bool ttHit, givesCheck, evasionPrunable;
    Depth ttDepth;
    int moveCount;

    if (PvNode)
    {
        oldAlpha = alpha; // To flag BOUND_EXACT when eval above alpha and no available moves
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    ss->currentMove = bestMove = MOVE_NONE;
    (ss+1)->ply = ss->ply + 1;
    moveCount = 0;

    // Check for an instant draw or if the maximum ply has been reached
    if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return ss->ply >= MAX_PLY && !InCheck ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = InCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                  : DEPTH_QS_NO_CHECKS;
    // Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ttHit);
    ttMove = ttHit ? tte->move() : MOVE_NONE;
    ttValue = ttHit ? value_from_tt(tte->value(), ss->ply) : VALUE_NONE;

    if (  !PvNode
        && ttHit
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race
        && (ttValue >= beta ? (tte->bound() &  BOUND_LOWER)
                            : (tte->bound() &  BOUND_UPPER)))
        return ttValue;

    // Evaluate the position statically
    if (InCheck)
    {
        ss->staticEval = VALUE_NONE;
        bestValue = futilityBase = -VALUE_INFINITE;
    }
    else
    {
        if (ttHit)
        {
            // Never assume anything on values stored in TT
            if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                ss->staticEval = bestValue = evaluate(pos);

            // Can ttValue be used as a better position evaluation?
            if (   ttValue != VALUE_NONE
                && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        }
        else
            ss->staticEval = bestValue =
            (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                             : -(ss-1)->staticEval + 2 * Eval::Tempo;

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, ss->staticEval, TT.generation());

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = bestValue + 128;
    }

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions and checks (only if depth >= DEPTH_QS_CHECKS) will
    // be generated.
    MovePicker mp(pos, ttMove, depth, &pos.this_thread()->mainHistory, &pos.this_thread()->captureHistory, to_sq((ss-1)->currentMove));

    // Loop through the moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move()) != MOVE_NONE)
    {
      assert(is_ok(move));

      givesCheck =  type_of(move) == NORMAL && !pos.discovered_check_candidates()
                  ? pos.check_squares(type_of(pos.moved_piece(move))) & to_sq(move)
                  : pos.gives_check(move);

      moveCount++;

      // Futility pruning
      if (   !InCheck
          && !givesCheck
          &&  futilityBase > -VALUE_KNOWN_WIN
          && !pos.advanced_pawn_push(move))
      {
          assert(type_of(move) != ENPASSANT); // Due to !pos.advanced_pawn_push

          futilityValue = futilityBase + PieceValue[EG][pos.piece_on(to_sq(move))];

          if (futilityValue <= alpha)
          {
              bestValue = std::max(bestValue, futilityValue);
              continue;
          }

          if (futilityBase <= alpha && !pos.see_ge(move, VALUE_ZERO + 1))
          {
              bestValue = std::max(bestValue, futilityBase);
              continue;
          }
      }

      // Detect non-capture evasions that are candidates to be pruned
      evasionPrunable =    InCheck
                       &&  (depth != DEPTH_ZERO || moveCount > 2)
                       &&  bestValue > VALUE_MATED_IN_MAX_PLY
                       && !pos.capture(move);

      // Don't search moves with negative SEE values
      if (  (!InCheck || evasionPrunable)
          &&  !pos.see_ge(move))
          continue;

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Check for legality just before making the move
      if (!pos.legal(move))
      {
          moveCount--;
          continue;
      }

      ss->currentMove = move;

      // Make and search the move
      pos.do_move(move, st, givesCheck);
      value = givesCheck ? -qsearch<NT,  true>(pos, ss+1, -beta, -alpha, depth - ONE_PLY)
                         : -qsearch<NT, false>(pos, ss+1, -beta, -alpha, depth - ONE_PLY);
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Check for a new best move
      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
			  bestMove = move;
			  
              if (PvNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha here!
                  alpha = value;
			 else
                  break; // Fail high

          }
       }
    }

    // All legal moves have been searched. A special case: If we're in check
    // and no legal moves were found, it is checkmate.
    if (InCheck && bestValue == -VALUE_INFINITE)
        return mated_in(ss->ply); // Plies to mate from the root

    tte->save(posKey, value_to_tt(bestValue, ss->ply),
              bestValue >= beta ? BOUND_LOWER :
              PvNode && bestValue > oldAlpha  ? BOUND_EXACT : BOUND_UPPER,
              ttDepth, bestMove, ss->staticEval, TT.generation());

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate score from "plies to mate from the root" to
  // "plies to mate from the current position". Non-mate scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v >= VALUE_MATE_IN_MAX_PLY  ? v + ply
          : v <= VALUE_MATED_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): It adjusts a mate score
  // from the transposition table (which refers to the plies to mate/be mated
  // from current position) to "plies to mate/be mated from the root".

  Value value_from_tt(Value v, int ply) {

    return  v == VALUE_NONE             ? VALUE_NONE
          : v >= VALUE_MATE_IN_MAX_PLY  ? v - ply
          : v <= VALUE_MATED_IN_MAX_PLY ? v + ply : v;
  }


  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, and -4 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 4})
        if (is_ok((ss-i)->currentMove))
            (ss-i)->contHistory->update(pc, to, bonus);
  }


  // update_capture_stats() updates move sorting heuristics when a new capture best move is found

  void update_capture_stats(const Position& pos, Move move,
                            Move* captures, int captureCnt, int bonus) {

      CapturePieceToHistory& captureHistory =  pos.this_thread()->captureHistory;
      Piece moved_piece = pos.moved_piece(move);
      PieceType captured = type_of(pos.piece_on(to_sq(move)));
      captureHistory.update(moved_piece, to_sq(move), captured, bonus);

      // Decrease all the other played capture moves
      for (int i = 0; i < captureCnt; ++i)
      {
          moved_piece = pos.moved_piece(captures[i]);
          captured = type_of(pos.piece_on(to_sq(captures[i])));
          captureHistory.update(moved_piece, to_sq(captures[i]), captured, -bonus);
      }
  }


  // update_stats() updates move sorting heuristics when a new quiet best move is found

  void update_stats(const Position& pos, Stack* ss, Move move,
                    Move* quiets, int quietsCnt, int bonus) {

    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color c = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory.update(c, move, bonus);
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }

    // Decrease all the other played quiet moves
    for (int i = 0; i < quietsCnt; ++i)
    {
        thisThread->mainHistory.update(c, quiets[i], -bonus);
        update_continuation_histories(ss, pos.moved_piece(quiets[i]), to_sq(quiets[i]), -bonus);
    }
  }


  // Is the PV leading to a draw position? Assumes all pv moves are legal
  bool pv_is_draw(Position& pos) {

    StateInfo st[MAX_PLY];
    auto& pv = pos.this_thread()->rootMoves[0].pv;

    for (size_t i = 0; i < pv.size(); ++i)
        pos.do_move(pv[i], st[i]);

    bool isDraw = pos.is_draw(pv.size());

    for (size_t i = pv.size(); i > 0; --i)
        pos.undo_move(pv[i-1]);

    return isDraw;
  }

} // namespace

  // check_time() is used to print debug info and, more importantly, to detect
  // when we are out of available time and thus stop the search.

  void MainThread::check_time() {

    if (--callsCnt > 0)
        return;

    // At low node count increase the checking rate to about 0.1% of nodes
    // otherwise use a default value.
    callsCnt = Limits.nodes ? std::min(4096, int(Limits.nodes / 1024)) : 4096;

    static TimePoint lastInfoTime = now();

    int elapsed = Time.elapsed();
    TimePoint tick = Limits.startTime + elapsed;

    if (tick - lastInfoTime >= 1000)
    {
        lastInfoTime = tick;
        dbg_print();
    }

    // An engine may not stop pondering until told so by the GUI
    if (Threads.ponder)
        return;

    if (   (Limits.use_time_management() && elapsed > Time.maximum() - 10)
        || (Limits.movetime && elapsed >= Limits.movetime)
        || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
            Threads.stop = true;
  }


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth, Value alpha, Value beta) {

  std::stringstream ss;
  int elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t PVIdx = pos.this_thread()->PVIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = (i <= PVIdx && rootMoves[i].score != -VALUE_INFINITE);

      if (depth == ONE_PLY && !updated)
          continue;

      Depth d = updated ? depth : depth - ONE_PLY;
      Value v = updated ? rootMoves[i].score : rootMoves[i].previousScore;


      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";

      ss << "info"
         << " depth "    << d / ONE_PLY
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v);

      if (i == PVIdx)
          ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed;

      if (elapsed > 1000) // Earlier makes little sense
          ss << " hashfull " << TT.hashfull();

      ss << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(m, pos.is_chess960());
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    bool ttHit;

    assert(pv.size() == 1);

    if (!pv[0])
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

