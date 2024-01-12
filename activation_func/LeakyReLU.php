<?php

// Activate Function
function LeakyReLU($sum)
{
    return ($sum < 0) ? 0.1 * $sum : $sum;
}
