<?php

// Activate Function
function sigmoid($sum)
{
    return 1 / (1 + exp(-$sum));
}
