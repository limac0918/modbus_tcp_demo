#!/bin/bash
set -e

cd "$(dirname "$0")"

# 0. Docker 环境检查
echo "[check] Checking docker environment..."
if ! command -v docker &> /dev/null; then
    echo "[error] docker not found. Install with: sudo apt install docker.io"
    exit 1
fi
if ! docker info &> /dev/null; then
    echo "[error] docker daemon not running or permission denied."
    echo "  Start: sudo systemctl start docker"
    echo "  Add user: sudo usermod -aG docker $USER && newgrp docker"
    exit 1
fi
echo "[check] docker OK."

# 1. 判断是否需要重新编译
need_build=0
if [ ! -f gateway ]; then
    echo "[check] gateway binary not found, will build."
    need_build=1
elif [ gateway.c -nt gateway ]; then
    echo "[check] gateway.c newer than gateway, will rebuild."
    need_build=1
elif ldd gateway 2>&1 | grep -q "not found"; then
    echo "[check] gateway has missing libraries, will rebuild."
    need_build=1
else
    echo "[check] gateway binary is up to date, skip build."
fi

# 2. 编译（如果需要）
if [ $need_build -eq 1 ]; then
    echo "[1/4] Building gateway..."
    rm -f gateway
    gcc -std=c9x gateway.c \
        $(pkg-config --cflags --libs open62541 libmodbus) \
        -lpthread -o gateway
else
    echo "[1/4] Skip build."
fi

# 3. 解析依赖
echo "[2/4] Resolving shared libraries..."
rm -rf docker_libs
mkdir -p docker_libs
LIBS=$(ldd gateway | grep -v -E 'linux-vdso|libc\.|libpthread|libm\.|ld-linux|libdl\.|librt\.' \
    | grep '=>' | awk '{print $3}')
for lib in $LIBS; do
    echo "  copy: $lib"
    cp "$lib" docker_libs/
done

# 4. Docker build
echo "[3/4] Building docker image..."
docker build -t modbus-opcua-gateway .

# 5. 清理
rm -rf docker_libs
echo "[4/4] Cleaned up."

echo "Done! Run with:"
echo "  docker run --rm -it --network host --name gateway modbus-opcua-gateway"
#（注：内容由AI生成）
