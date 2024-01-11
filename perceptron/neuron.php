<?php

// inputs
$x = array(2, 3);
// weights
$w = array(0.7, 0.6);
//bias
$b = 1;

$sum = $x[0] * $w[0] + $x[1] * $w[1] + $b;

// ReLU Activation Function
if ($sum > 0) {
    echo $sum;
} else {
    echo 0;
}
