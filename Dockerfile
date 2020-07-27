FROM debian:10-slim

# RUN apt-get update && apt-get install build-essential

# Install kplex
COPY . /app
RUN make /app
RUN make install /app

# Remove source code
RUN rm -r /app

# Entrypoint
CMD /usr/bin/kplex -o mode=background