<?php

// Activate Function
function tan_h($sum)
{
    return (exp($sum) - exp(-$sum)) / (exp($sum) + exp(-$sum));
}
