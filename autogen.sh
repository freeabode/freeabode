#!/bin/sh
# autogen.sh

set -ex

mkdir -p ac-aux

autoreconf -v -f -i -W all

rm -rf autom4te.cache

# end autogen.sh
