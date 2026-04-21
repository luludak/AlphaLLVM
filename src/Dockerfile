# Alpha Compiler — Reproducible build image
# Build: docker build -t alpha-compiler .
# Run:   docker run --rm -v $(pwd):/src alpha-compiler /src/test.alpha

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    flex \
    bison \
    clang \
    llvm-dev \
    libllvm-ocaml-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /compiler
COPY . .

RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc)

# Copy binaries to /usr/local
RUN cp build/alphac /usr/local/bin/ && \
    cp build/libalpharuntime.a /usr/local/lib/

# Entry point: compile & run an Alpha source file
ENTRYPOINT ["/bin/bash", "-c", \
    "alphac --emit-llvm \"$1\" && \
     llc -filetype=obj -o /tmp/prog.o \"${1%.alpha}.ll\" && \
     clang /tmp/prog.o /usr/local/lib/libalpharuntime.a -lm -o /tmp/prog && \
     /tmp/prog", "--"]
