<?php

function binaray_cross_entropy($y, $y_pred)
{

    $c = array_map(function ($t, $p) {
        return -($t * log($p) + (1 - $t) * log(1 - $p));
    }, $y, $y_pred);

    return array_sum($c) / count($c);
}

function cross_entropy($y, $y_pred)
{

    $c = array_map(function ($t, $p) {
        return -($t * log($p, 2));
    }, $y, $y_pred);

    return array_sum($c) ;
}


$p = array(1.0, 0.0, 0.0, 1.0);

$q = array(0.8, 0.5, 0.6, 0.4);


echo cross_entropy($p, $q);
echo "<br>";
echo binaray_cross_entropy($p, $q);
