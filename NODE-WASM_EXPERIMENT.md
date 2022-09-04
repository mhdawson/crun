# NODE-WASM-EXPERIMENT

Experiment of integration Node.js to run wasm containers by
hacking wastime integration

# Node version

The experiment uses the code from the
[Extend Node-API to libnode](https://github.com/nodejs/node/pull/43542)
PR. This was an easy way to get a set of C functions that would be integrated
with crun that is C versus C++.  That PR is likely a long way (if ever from
landing) but reduced the work that it would have taken to build C wrapper
using the Node.js C++ embedder API or converting the crun project over
to C++.

Node needs to be built with

```
./configure --shared
``

And the the resulting `../out/Release/libnode.so.108` copied to
libnode.so in the node-lib directory added to this branch.

The following files also need to be copied from `../src` directory
in the Node.js project to the node-include directory added to this branch

* js_native_api.h
* js_native_api_types.h
* node_api.h
* node_api_types.h

**NOTE:** I've included these as part of the PR along with the shared library
so that everybody does not need to build the node shared library
and extract the associated headers. May or may not work on the
machine you run on. I built/ran on Fedora 36.

# Building crun

Before building running you will need to so the following:
* copy the headers node-include/* into /usr/include
* copy node-lib/libnode.so to /lib64

The file `buildit.sh` shows the steps used to build with
node.js integrated.

# Building wasm container

The wasm to be run must be built an pushed to the local repository
and be tagged as a wasm container.  See
[wasmtime-tutorial](https://github.com/font/wasmtime-tutorial.git)
for more details.

My specific stesp to build were as follows:

```shell
cargo build --target wasm32-wasi --release
chomd +x ./target/wasm32-wasi/release/hello_wasm.wasm
buildah build  --annotation "module.wasm.image/variant=compat" -t wasm-test -f ./Containerfile
```

with the resulting container being:

```shell
REPOSITORY           TAG         IMAGE ID      CREATED       SIZE
localhost/wasm-test  latest      8c9b454570f0  20 hours ago  2.01 MB
```

# Running

The file `runit.sh` has the steps I used to run. It hard codes
the path to the updated crun to
```
/root/wasmtime-tutorial/crun/crun`
```

which will need to be updated for your path if you use the script





