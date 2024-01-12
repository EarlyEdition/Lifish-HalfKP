# Machine Learning

Machine Learning is a subfield of computer science that gives computers the ability to learn without being programmed.

## Perceptrons

<img src="https://raw.githubusercontent.com/WampServer/Machine-Learning/master/perceptron/neuron.png" width="50%" align="left"></img>

Perceptron is the simplest possible Neural Network. A neuron takes inputs, does some math with them, and produces one output.

Each input is multiplied by a weight and all the weighted inputs are added together with a bias:
####  x1​.w1 + x2​.w2 + x3.w3​ + b

The sum is passed through an activation function:
#### y = f(x1​.w1​+x2​.w2​+x3.w3​+b)

## Activation Function
The activation function maps the weighted sum in between 0 to 1 or -1 to 1 etc. It is used to determine the output of neural network like yes or no. Two commonly activation functions are sigmoid and tanh:

<img src="https://raw.githubusercontent.com/WampServer/Machine-Learning/master/activation_func/sigmoid.png" width="40%" align="left"></img>

<img src="https://raw.githubusercontent.com/WampServer/Machine-Learning/master/activation_func/tanh.png" width="40%" ></img>

## Loss Function
The loss function uses the weight and bias from the model and returns an error, based on how well the line fits a plot. It measures how well the neural network models the training data. When training, we aim to minimize this loss between the predicted and target outputs.A commonly used loss function is MSE (Mean Squared Error):


<img src="https://raw.githubusercontent.com/WampServer/Machine-Learning/master/loss_func/Mean-Squared-Error.png" width="50%" align="left" ></img>
#### y​ is the true value of the variable.

#### ŷ is the predicted value of the variable. 

#### n is the number of samples.

## Gradient Descent

Gradient Descent is a popular algorithm for solving AI problems. It tells us how to change our weights and biases to minimize loss. A commonly optimization algorithm called stochastic gradient descent (SGD)

<img src="https://raw.githubusercontent.com/WampServer/Machine-Learning/master/gradient_descent/SGD.png" width="50%" align="left" ></img>

#### w(new) = w(old)​−α.​∂L​/∂w

#### α is the learning rate.

#### ∂L/∂w is derivative of the loss function.
