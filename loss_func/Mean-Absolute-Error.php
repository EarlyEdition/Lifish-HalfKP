<?php

// Loss Function
function mae($y, $y_pred)
{

    $c = array_map(function ($a, $b) {
        return abs($a - $b);
    }, $y, $y_pred);

    return array_sum($c) / count($c);
}

$y1 = array(2,4,6,8,10);

$y2 = array(2,3,5,7,11);

echo mae($y1, $y2);
