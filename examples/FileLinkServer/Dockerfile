FROM debian:bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential \
    cmake \
    pkg-config \
    libmysqlcppconn-dev \
    libhiredis-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN cmake -S /app -B /app/build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /app/build -j

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    libmysqlcppconn7v5 \
    libhiredis0.14 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /app/build/examples/FileLinkServer/FileLinkServer /usr/local/bin/FileLinkServer

EXPOSE 8080

# 默认使用挂载的 /config
CMD ["/usr/local/bin/FileLinkServer", "/config/"]
