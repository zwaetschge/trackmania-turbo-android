# Settings of the developer scripts (tools/device*.sh, host*.sh, dock.sh, sync_android.sh);
# ./build.sh does not need any of this. Sourced from the repository root. Values come from
# the environment or from work/local.env (not tracked), for example:
#   TM_HOST_ROOT=/path/of/this/checkout/as/the/docker/daemon/sees/it   (default: this directory)
#   TM_GAME_DIR="/path/to/Trackmania Turbo"            (host runs: tools/hostgo.sh, hostrun.sh)
#   TM_SERIAL=<adb serial of the test device>
#   TM_CAPTURE_URL=http://<this machine>:8766          (tools/lan_recv.py receives device captures)
#   TM_BUILDER_PROJ=<android-builder project id>  TM_BUILDER_DIR=<that project's workspace>
# tools/device.sh drives the device through the android-builder MCP of the Plum environment
# (node /app/scripts/mcp-call.mjs); elsewhere replace its mcp()/ash() helpers with adb.
[ -f work/local.env ] && . ./work/local.env
TM_HOST_ROOT=${TM_HOST_ROOT:-$PWD}
