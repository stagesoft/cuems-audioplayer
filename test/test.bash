#!/bin/bash

executable=/home/ion/src/cuems/cuems-audioplayer/build/audioplayer-cuems
start_port=8000
instancias=20
contador=1
while [ $contador -le $instancias ]; do
  num=$((start_port+contador))
  $executable -f ~/Music/dummy-message.wav -p $num --mtcfollow &
  contador=$((contador + 1))
  sleep 0.8  # To prevent too many simultaneous connections to the server.

done