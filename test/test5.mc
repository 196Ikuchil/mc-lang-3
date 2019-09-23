def fib(x)
  # TODO 3.5
  if x < 3 then
    1
  else
    fib(x - 1) + fib(x - 2)
