#!/bin/sh

#
# SPDX-FileCopyrightText: 2014-2021 Milian Wolff <mail@milianw.de>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#

usage() {
    echo "Usage: $0 [--debug|-d] [--use-inject] [--record-only] DEBUGGEE [ARGUMENT]..."
    echo "or:    $0 [--debug|-d] -p PID"
    echo "or:    $0 -a FILE"
    echo
    echo "A heap memory usage profiler. It uses the dynamic loader to track all"
    echo "calls to the core memory allocation functions and logs these"
    echo "occurrences. Additionally, backtraces are obtained and logged."
    echo "Combined this can give interesting answers to questions such as:"
    echo
    echo "  * How much heap memory is my application using?"
    echo "  * Where is heap memory being allocated, and how often?"
    echo "  * How much space are heap individual allocations requesting?"
    echo
    echo "To evaluate the generated heaptrack data, use heaptrack_print or heaptrack_gui."
    echo
    echo "Mandatory arguments to heaptrack:"
    echo "  DEBUGGEE       The name or path to the application that should"
    echo "                 be run with heaptrack analyzation enabled."
    echo
    echo "Alternatively, to attach to a running process:"
    echo "  -p, --pid PID  The process ID of a running process into which"
    echo "                 heaptrack will be injected. This only works with"
    echo "                 applications that already link against libdl."
    echo "  WARNING: Runtime-attaching heaptrack is UNSTABLE and can lead to CRASHES"
    echo "           in your application, especially after you detach heaptrack again."
    echo "           You are hereby warned, use it at your own risk!"
    echo
    echo "Optional arguments to heaptrack:"
    echo "  -r, --raw      Only record raw data, do not interpret it."
    echo "  -d, --debug    Run the debuggee in GDB and heaptrack."
    echo " --use-inject    Use the same heaptrack_inject symbol interception mechanism instead of relying on"
    echo "                 the dynamic linker and LD_PRELOAD. This is an experimental flag for now."
    echo " --asan          Enables running heaptrack on binaries built with gcc's address sanitizer enabled."
    echo "                 Implies --use-inject."
    echo " --record-only   Only record and interpret the data, do not attempt to analyze it."
    echo "  ARGUMENT       Any number of arguments that will be passed verbatim"
    echo "                 to the debuggee."
    echo "  -h, --help     Show this help message and exit."
    echo "  -v, --version  Displays version information."
    echo "  -q, --quiet    Only print error messages."
    echo "  -o, --output   Specifies the data-file for the captured data."
    echo "                 %h in the file name string is replaced with the hostname of the system."
    echo "                 %p in the file name string is replaced with the pid of the application being profiled."
    echo "                 Parent directories will be created if output files are under non-existing directories."
    echo "                 e.g.,"
    echo "                   ./%h/%p/outdat will be translated into ./<hostname>/<pid>/outdat."
    echo "                   The directory ./<hostname>/<pid> will be created if it doesn't exist."
    echo
    echo "Alternatively, to interpret a raw recorded heaptrack data file:"
    echo "  -i, --interpret FILE  Convert a raw heaptrack data file with heaptrack_interpret."
    echo "                        Any options passed after --analyze will be passed along."
    echo
    echo "Alternatively, to analyze a recorded heaptrack data file:"
    echo "  -a, --analyze FILE    Open the heaptrack data file in heaptrack_gui, if available,"
    echo "                        or fallback to heaptrack_print otherwise."
    echo "                        Any options passed after --analyze will be passed along."
    echo
    exit 0
}

debug=
pid=
client=
use_inject_lib=
write_raw_data=
record_only=
asan=
asan_ld_preload=
quiet=
output=
platform="@CMAKE_SYSTEM_NAME@"

resolve_path() {
    path="$1"
    if resolved=$(readlink -f "$path" 2> /dev/null) && [ -n "$resolved" ]; then
        printf '%s\n' "$resolved"
    elif [ -e "$path" ] && command -v realpath > /dev/null 2>&1; then
        realpath "$path"
    elif [ -e "$path" ] && command -v perl > /dev/null 2>&1; then
        perl -MCwd=abs_path -e 'print abs_path(shift)' "$path"
    else
        directory=$(dirname "$path")
        filename=$(basename "$path")
        printf '%s/%s\n' "$(cd "$directory" && pwd -P)" "$filename"
    fi
}

# path to current heaptrack.sh executable
SCRIPT_PATH=$(resolve_path "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
EXE_PATH=$(resolve_path "$SCRIPT_DIR")

# find preload library and interpreter executable using relative paths
LIB_REL_PATH="@LIB_REL_PATH@"
LIBEXEC_REL_PATH="@LIBEXEC_REL_PATH@"

INTERPRETER="$EXE_PATH/$LIBEXEC_REL_PATH/heaptrack_interpret"
INTERPRETER=$(resolve_path "$INTERPRETER")

GZ_COMPRESSOR="gzip -c"
GZ_UNCOMPRESSOR="gzip -dc"

ZSTD_COMPRESSOR="zstd -c"
ZSTD_UNCOMPRESSOR="zstd -dc"

COMPRESSOR="$GZ_COMPRESSOR"
UNCOMPRESSOR="$GZ_UNCOMPRESSOR"
output_suffix="gz"
if [ "@ZSTD_FOUND@" = "TRUE" ] && [ ! -z "$(command -v zstd 2> /dev/null)" ]; then
    output_suffix="zst"
    COMPRESSOR="$ZSTD_COMPRESSOR"
    UNCOMPRESSOR="$ZSTD_UNCOMPRESSOR"
fi

interpretRawHeaptrackDataFile() {
    input="$1"
    shift 1

    if  [ ! -f "$INTERPRETER" ]; then
        echo "Could not find heaptrack interpreter executable: $INTERPRETER"
        exit 1
    fi

    if [ ! -f "$input" ]; then
        echo "raw file "$input" does not exist"
        exit 1
    fi

    output=$(echo $input | sed -e 's/.zst$//' -e 's/.gz$//' -e 's/.raw$//')
    output="$output.$output_suffix"
    echo "writing interpreted data to $output..."

    case "$input" in
        *.gz)
            $GZ_UNCOMPRESSOR < "$input" | "$INTERPRETER" "$@" | $COMPRESSOR > "$output"
            ;;
        *.zst)
            $ZSTD_UNCOMPRESSOR < "$input" | "$INTERPRETER" "$@" | $COMPRESSOR > "$output"
            ;;
        *)
            "$INTERPRETER" "$@" | $COMPRESSOR > "$output"
            ;;
    esac

    echo
    echo "Interpretation finished, you can now analyze the data:"
    echo
    echo "  heaptrack --analyze \"$output\""
}

openHeaptrackDataFiles() {
    if [ -x "$EXE_PATH/heaptrack_gui" ]; then
        "$EXE_PATH/heaptrack_gui" "$@"
    else
        "$EXE_PATH/heaptrack_print" "$@"
    fi
}

ORIG_CMDLINE=$@

while true; do
    case "$1" in
        "-d" | "--debug")
            if [ -z "$(command -v gdb 2> /dev/null)" ]; then
                echo "GDB is not installed, cannot debug heaptrack."
                exit 1
            fi
            debug=1
            shift 1
            ;;
        "--use-inject")
            if [ "$platform" = "Darwin" ]; then
                echo "heaptrack: --use-inject is not supported on macOS" >&2
                exit 1
            fi
            use_inject_lib=1
            shift 1
            ;;
        "-r" | "--raw")
            write_raw_data=1
            shift 1
            ;;
        "--asan")
            if [ "$platform" = "Darwin" ]; then
                echo "heaptrack: --asan is not supported on macOS" >&2
                exit 1
            fi
            asan=1
            use_inject_lib=1
            shift 1
            ;;
        "--record-only")
            record_only=1
            shift 1
            ;;
        "-h" | "--help")
            usage
            exit 0
            ;;
        "-o" | "--output" | "--output-file")
            if [ -z "$2" ]; then
                echo "Missing output argument."
                exit 1
            fi
            output=$(printf '%s\n' "$2" | sed "s/%h/$(hostname)/g" | sed "s/%p/$$/g")
            if [ -d "$output" ]; then
                echo "Please specify a file-name or a full path-name for output."
                exit 1
            fi
            output_directory=$(dirname "$output")
            if [ ! -d "$output_directory" ]; then
              mkdir -p "$output_directory"
            fi
            output=$(resolve_path "$output")
            shift 2
            ;;
        "-p" | "--pid")
            if [ "$platform" = "Darwin" ]; then
                echo "heaptrack: --pid is not supported on macOS" >&2
                exit 1
            fi
            if [ -z "$(command -v gdb 2> /dev/null)" ]; then
                echo "GDB is not installed, cannot attach to running process."
                exit 1
            fi
            if [ -f "/proc/sys/kernel/yama/ptrace_scope"  ] && [ "$(cat "/proc/sys/kernel/yama/ptrace_scope")" -gt "0" ]; then
                echo "Cannot runtime-attach, you need to set /proc/sys/kernel/yama/ptrace_scope to 0"
                exit 1
            fi
            pid=$2
            if [ -z "$pid" ]; then
                echo "Missing PID argument."
                exit 1
            fi
            case $(uname) in
                Linux*)
                    client=$(cat "/proc/$pid/comm")
                ;;
                FreeBSD*)
                    client=$(awk '{print $1}' < "/proc/$pid/cmdline")
                ;;
            esac
            if [ -z "$client" ]; then
                echo "Cannot attach to unknown process with PID $pid."
                exit 1
            fi
            shift 2
            echo $@
            if [ ! -z "$@" ]; then
                echo "You cannot specify a debuggee and a pid at the same time."
                exit 1
            fi
            break
            ;;
        "-q" | "--quiet")
            quiet=1
            shift
            ;;
        "-v" | "--version")
            echo "heaptrack @HEAPTRACK_VERSION_MAJOR@.@HEAPTRACK_VERSION_MINOR@.@HEAPTRACK_VERSION_PATCH@"
            exit 0
            ;;
        "-i" | "--interpret")
            shift 1
            interpretRawHeaptrackDataFile "$@"
            exit
            ;;
        "-a" | "--analyze")
            shift 1
            openHeaptrackDataFiles "$@"
            exit
            ;;
        *)
            if [ "$1" = "--" ]; then
                shift 1
            fi
            if [ ! -x "$(command -v "$1" 2> /dev/null)" ]; then
                if [ -z "$1" ] && [ -x "$EXE_PATH/heaptrack_gui" ]; then
                    "$EXE_PATH/heaptrack_gui"
                    exit
                fi
                if [ -f "$1" ] && echo "$1" | grep -q "heaptrack."; then
                    openHeaptrackDataFiles "$ORIG_CMDLINE"
                    exit
                fi

                if [ ! -e "$1" ]; then
                    echo "Error: Debuggee \"$1\" was not found."
                else
                    echo "Error: Debuggee \"$1\" is not an executable."
                fi

                echo
                echo "Usage: $0 [--debug|-d] [--help|-h] DEBUGGEE [ARGS...]"
                exit 1
            fi
            client="$1"
            shift 1
            break
            ;;
    esac
done

# put output into current pwd
if [ -z "$output" ]; then
    output=$(pwd)/heaptrack.$(basename "$client").$$
fi

ENVCHECKER="$EXE_PATH/$LIBEXEC_REL_PATH/heaptrack_env"
if [ ! -f "$ENVCHECKER" ]; then
    echo "Could not find heaptrack_env: $ENVCHECKER"
    exit 1
fi
ENVCHECKER=$(resolve_path "$ENVCHECKER")

if [ -z "$use_inject_lib" ]; then
    LIBHEAPTRACK_PRELOAD="$EXE_PATH/$LIB_REL_PATH/libheaptrack_preload@CMAKE_SHARED_LIBRARY_SUFFIX@"
else
    LIBHEAPTRACK_PRELOAD="$EXE_PATH/$LIB_REL_PATH/libheaptrack_inject@CMAKE_SHARED_LIBRARY_SUFFIX@"
fi
if [ ! -f "$LIBHEAPTRACK_PRELOAD" ]; then
    echo "Could not find heaptrack preload library $LIBHEAPTRACK_PRELOAD"
    exit 1
fi
LIBHEAPTRACK_PRELOAD=$(resolve_path "$LIBHEAPTRACK_PRELOAD")

LIBHEAPTRACK_INJECT=
if [ "$platform" != "Darwin" ]; then
    LIBHEAPTRACK_INJECT="$EXE_PATH/$LIB_REL_PATH/libheaptrack_inject@CMAKE_SHARED_LIBRARY_SUFFIX@"
    if [ ! -f "$LIBHEAPTRACK_INJECT" ]; then
        echo "Could not find heaptrack inject library $LIBHEAPTRACK_INJECT"
        exit 1
    fi
    LIBHEAPTRACK_INJECT=$(resolve_path "$LIBHEAPTRACK_INJECT")
fi

if [ -n "$asan" ]; then
  # We need to check the actual path to the binary
  bin_path=$(resolve_path "/proc/$pid/exe")
  asan_ld_preload=$(ldd $bin_path | grep libasan | sed -e 's/.*=> //;s/ (.*//')
  if [ -z "$asan_ld_preload" ]; then
    echo "Unable to detect libasan when running ldd on the executable $client"
    exit 1
  fi
  echo "Found ASAN library: $asan_ld_preload"
  asan_ld_preload="$asan_ld_preload:"
fi

# Set up private IPC paths. The early trap covers every failure after this point.
runtime_dir=
pipe=
ready_file=
interpreted_pipe=
background_pids=

cleanup_runtime() {
    for background_pid in $background_pids; do
        kill "$background_pid" 2> /dev/null || true
    done
    [ -n "$pipe" ] && rm -f "$pipe" "$pipe.lock"
    [ -n "$ready_file" ] && rm -f "$ready_file"
    [ -n "$interpreted_pipe" ] && rm -f "$interpreted_pipe"
    [ -n "$runtime_dir" ] && rmdir "$runtime_dir" 2> /dev/null || true
}

old_umask=$(umask)
umask 077
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/heaptrack.XXXXXX") || {
    umask "$old_umask"
    echo "heaptrack: failed to create a private runtime directory" >&2
    exit 1
}
trap cleanup_runtime EXIT INT TERM

pipe="$runtime_dir/data"
if ! mkfifo "$pipe"; then
    umask "$old_umask"
    echo "heaptrack: failed to create the tracking FIFO" >&2
    exit 1
fi
if [ "$platform" = "Darwin" ]; then
    ready_file="$runtime_dir/ready"
    : > "$ready_file"
fi
umask "$old_umask"

# if root is profiling a process for non root
# give profiled process write access to the pipe
if [ ! -z "$pid" ]; then
  case $(uname) in
    Linux*)
      pid_user=$(stat -c %u "/proc/$pid")
    ;;
    FreeBSD*)
      pid_user=$(stat -f %Su "/proc/$pid")
    ;;
    Darwin*)
      pid_user=$(stat -f %u "/proc/$pid")
    ;;
  esac
  if [ -z "$pid_user" ]; then
    exit 1
  fi
  chown "$pid_user" "$runtime_dir" "$pipe" || exit 1
fi

output_suffix="gz"
COMPRESSOR="gzip -c"
UNCOMPRESSOR="gzip -dc"

if [ "@ZSTD_FOUND@" = "TRUE" ] && [ ! -z "$(command -v zstd 2> /dev/null)" ]; then
    output_suffix="zst"
    COMPRESSOR="zstd -c"
    UNCOMPRESSOR="zstd -dc"
fi

output_no_suffix="$output"
output_non_raw="$output.$output_suffix"

if [ ! -z "$write_raw_data" ]; then
    output_suffix="raw.$output_suffix"
fi

# interpret the data and compress the output on the fly
output="$output.$output_suffix"
if [ -z "$write_raw_data" ]; then
    if  [ ! -f "$INTERPRETER" ]; then
        echo "Could not find heaptrack interpreter executable: $INTERPRETER"
        exit 1
    fi
    interpreted_pipe="$runtime_dir/interpreted"
    if ! mkfifo "$interpreted_pipe"; then
        echo "heaptrack: failed to create the interpreter FIFO" >&2
        exit 1
    fi
    $COMPRESSOR < "$interpreted_pipe" > "$output" &
    compressor_pid=$!
    background_pids="$compressor_pid"
    "$INTERPRETER" < "$pipe" > "$interpreted_pipe" &
    interpreter_pid=$!
    background_pids="$interpreter_pid $compressor_pid"
else
    $COMPRESSOR < "$pipe" > "$output" &
    compressor_pid=$!
    background_pids="$compressor_pid"
fi

cleanup() {
    trap - EXIT INT TERM
    if [ ! -z "$pid" ] && [ -d "/proc/$pid" ]; then
        echo "removing heaptrack injection via GDB, this might take some time..."
        gdb --batch-silent -n -iex="set auto-solib-add off" \
            -iex="set language c" -p $pid \
            --eval-command="sharedlibrary libheaptrack_inject" \
            --eval-command="call (void) heaptrack_stop()" \
            --eval-command="detach"
        # NOTE: we do not call dlclose here, as that has the tendency to trigger
        #       crashes in the debuggee. So instead, we keep heaptrack loaded.
    fi
    cleanup_runtime

    if [ -z ${quiet} ]; then
      echo "Heaptrack finished! Now run the following to investigate the data:"
        echo

      if [ ! -z "$write_raw_data" ]; then
          echo "First, interpret the raw data (possibly specifying the sysroot etc):"
          echo
          echo "  heaptrack --interpret \"$output\""
          echo
          echo "Then, you can analyze it:"
          echo
      fi

      echo "  heaptrack --analyze \"$output\""
    fi

    if [ -z "$record_only" ] && [ -z "$write_raw_data" ] && [ -x "$EXE_PATH/heaptrack_gui" ]; then
        if [ -z ${quiet} ]; then
          echo ""
          echo "heaptrack_gui detected, automatically opening the file..."
        fi
        "$EXE_PATH/heaptrack_gui" "$output"
    fi
}
trap cleanup EXIT INT TERM

if [ -z ${quiet} ]; then
  echo "heaptrack output will be written to \"$output\""
fi

if [ -z "$debug" ] && [ -z "$pid" ]; then
  if [ -z ${quiet} ]; then
    echo "starting application, this might take some time..."
  fi
  if [ "$platform" = "Darwin" ]; then
    DYLD_INSERT_LIBRARIES="$LIBHEAPTRACK_PRELOAD${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}" \
      DUMP_HEAPTRACK_OUTPUT="$pipe" DUMP_HEAPTRACK_READY="$ready_file" "$client" "$@"
  else
    LD_PRELOAD="$asan_ld_preload$LIBHEAPTRACK_PRELOAD${LD_PRELOAD:+:$LD_PRELOAD}" \
      DUMP_HEAPTRACK_OUTPUT="$pipe" "$client" "$@"
  fi
  EXIT_CODE=$?
else
  if [ -z "$pid" ]; then
    if [ -z ${quiet} ]; then
      echo "starting application in GDB, this might take some time..."
    fi
    if [ "$platform" = "Darwin" ]; then
      preload_environment="DYLD_INSERT_LIBRARIES"
    else
      preload_environment="LD_PRELOAD"
    fi
    gdb --quiet --eval-command="set environment $preload_environment=$LIBHEAPTRACK_PRELOAD" \
        --eval-command="set environment DUMP_HEAPTRACK_OUTPUT=$pipe" \
        --eval-command="set environment DUMP_HEAPTRACK_READY=$ready_file" \
        --eval-command="set startup-with-shell off" \
        --eval-command="run" --args "$client" "$@"
    EXIT_CODE=$?
  else
    if [ -z ${quiet} ]; then
      echo "injecting heaptrack into application via GDB, this might take some time..."
    fi
    dlopen=$($ENVCHECKER dlopen "$LIBHEAPTRACK_INJECT")
    if [ -z "$debug" ]; then
        unset DEBUGINFOD_URLS
        gdb --batch-silent -n -iex="set auto-solib-add off" \
            -iex="set language c" -p $pid \
            --eval-command="sharedlibrary libc.so" \
            --eval-command="call (void) $dlopen" \
            --eval-command="sharedlibrary libheaptrack_inject" \
            --eval-command="call (void) heaptrack_inject(\"$pipe\")" \
            --eval-command="detach"
    else
        echo $dlopen
        gdb --quiet -iex="set language c" -p $pid \
            --eval-command="sharedlibrary libc.so" \
            --eval-command="print (void*) $dlopen" \
            --eval-command="sharedlibrary libheaptrack_inject" \
            --eval-command="call (void) heaptrack_inject(\"$pipe\")"
    fi
    EXIT_CODE=$?
    if [ -z ${quiet} ]; then
      echo "injection finished"
    fi
  fi
fi

if [ "$platform" = "Darwin" ] && [ -z "$pid" ] && [ ! -s "$ready_file" ]; then
  echo "heaptrack: macOS ignored DYLD_INSERT_LIBRARIES; System Integrity Protection may block this executable" >&2
  cleanup_runtime
  EXIT_CODE=1
fi

background_exit_code=0
for background_pid in $background_pids; do
  wait "$background_pid"
  wait_status=$?
  if [ "$wait_status" -ne 0 ]; then
    background_exit_code=$wait_status
  fi
done
background_pids=
if [ "$EXIT_CODE" -eq 0 ] && [ "$background_exit_code" -ne 0 ]; then
  EXIT_CODE=$background_exit_code
fi
exit $EXIT_CODE

# kate: hl Bash
