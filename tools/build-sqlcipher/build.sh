#!/bin/sh
# Build the on-device My Tag helper (scripts/device/mytags-onelib.c): SQLCipher (the unit has no SQLCipher, and
# exportLibrary.db is a SQLCipher database) and a static OpenSSL, both for the unit's hard-float ARM.
#
#   build-env/run.sh sh rblive4_sc/tools/build-sqlcipher/build.sh
#
# Output: rblive4_sc/work/sqlcipher/mytags-onelib (static, about 4 MB). Sources are downloaded into
# rblive4_sc/work/sqlcipher/src; nothing here is committed. Runs inside the rblive4-build container.
set -e
OPENSSL_VER=3.0.22
SQLCIPHER_VER=4.19.0
R=/work/rblive4_sc
OUT=$R/work/sqlcipher
CROSS=arm-linux-gnueabihf-
mkdir -p "$OUT/src"
cd "$OUT/src"

[ -f openssl.tar.gz ] || curl -fsSL -o openssl.tar.gz \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
[ -f sqlcipher.tar.gz ] || curl -fsSL -o sqlcipher.tar.gz \
    "https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v$SQLCIPHER_VER.tar.gz"
sha256sum openssl.tar.gz sqlcipher.tar.gz

# 1. OpenSSL: static libcrypto for ARM, no assembler (a few seconds slower to derive the key, but nothing to
#    go wrong), nothing the helper does not call.
if [ ! -f "$OUT/openssl/lib/libcrypto.a" ]; then
    rm -rf "openssl-$OPENSSL_VER"
    tar xzf openssl.tar.gz
    (cd "openssl-$OPENSSL_VER" &&
     ./Configure linux-armv4 no-shared no-asm no-tests no-dso no-engine no-ui-console no-comp \
         --cross-compile-prefix=$CROSS --prefix="$OUT/openssl" --libdir=lib &&
     make -j"$(nproc)" build_libs && make install_dev)
fi

# 2. SQLCipher amalgamation (sqlite3.c with the codec)
if [ ! -f "$OUT/sqlite3.c" ]; then
    rm -rf "sqlcipher-$SQLCIPHER_VER"
    tar xzf sqlcipher.tar.gz
    (cd "sqlcipher-$SQLCIPHER_VER" &&
     ./configure --disable-tcl >/dev/null &&
     make sqlite3.c >/dev/null &&
     cp sqlite3.c sqlite3.h "$OUT/")
fi

# 3. The helper
$CROSS"gcc" -O2 -static -w \
    -DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1 -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -I"$OUT" -I"$OUT/openssl/include" \
    -o "$OUT/mytags-onelib" "$R/scripts/device/mytags-onelib.c" "$OUT/sqlite3.c" \
    -L"$OUT/openssl/lib" -lcrypto -ldl -lpthread -lm
ls -l "$OUT/mytags-onelib"
