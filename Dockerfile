FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY include ./include
COPY src ./src
RUN make all

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/eventstore /usr/local/bin/eventstore
WORKDIR /data
ENTRYPOINT ["eventstore"]
