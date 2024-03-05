# Lifish+HalfKP

Lifish is a light chess engine based on Stockfish and the evaluation based on [efficiently updateable neural networks](https://github.com/EarlyEdition/HalfKP).

List of removed features in Lifish:

* Syzygy

* Skill Level

* Contempt

* ...

###


## UCI parameters

Currently, Lifish has the following UCI options:

  * #### Threads
    The number of CPU threads used for searching a position. For best performance, set
    this equal to the number of CPU cores available.

  * #### Hash
    The size of the hash table in MB.

  * #### MultiPV
    Output the N best lines (principal variations, PVs) when searching. Leave at 1 for best performance.

  * #### Move Overhead
    Assume a time delay of x ms due to network and GUI overheads. This is useful to avoid losses on time in those cases.

  * #### UCI_Chess960
    An option handled by your GUI. If true, Lifish will play Chess960.

  * #### Use NNUE
    Toggle between the NNUE and classical evaluation functions. If set to "true", the network parameters must be availabe to load from file (see also EvalFile).

  * #### EvalFile
    The name of the file of the NNUE evaluation parameters. Depending on the GUI the filename might have to include the full path to the folder/directory that contains the file.

## Compiling Lifish

The [MSYS2](https://www.msys2.org/) environment is recommended for compiling Lifish on Windows.

To compile, type:

    make target ARCH=arch [COMP=comp]

Example: `make build ARCH=x86-64 COMP=mingw`

Lists of supported targets, archs and compilers can be viewed by typing `make help`.
