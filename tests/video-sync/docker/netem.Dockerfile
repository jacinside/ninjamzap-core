# tc (iproute2) to add network delay/jitter to a test server container:
#   docker run --rm --net container:<server> --cap-add NET_ADMIN nz-netem:latest \
#     tc qdisc add dev eth0 root netem delay 120ms 80ms distribution normal
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends iproute2 && rm -rf /var/lib/apt/lists/*
