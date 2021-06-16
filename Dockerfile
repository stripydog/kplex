FROM debian:stretch-slim
LABEL "name"="kplex" \
  "description"="A multiplexer for various nmea 0183 interfaces"

ENV APP=/usr/src/app

WORKDIR $APP

COPY . $APP

RUN apt-get update && apt-get install -y \
  make \
  build-essential \
  pkg-config \
  && make \ 
  && apt-get remove -y make build-essential pkg-config \
  && apt-get autoremove -y \
  && rm -rf /var/lib/apt/lists/*

CMD $APP/kplex

