# devrun

[![CI](https://github.com/Mahalakshmiku2003/devrun/actions/workflows/ci.yml/badge.svg)](https://github.com/Mahalakshmiku2003/devrun/actions/workflows/ci.yml)

A small process supervisor for local development, written in C++17 on plain POSIX APIs (no libraries).
Describe your services in one file, run one command, and get ordered startup, health checks, one merged log stream, automatic restarts and clean shutdown.

<!-- DEMO GIF GOES HERE: ![demo](docs/demo.gif) -->

## The problem

Running a real app locally often means a database, an API, a worker and a frontend, each in its own terminal, started in the right order. Logs are scattered, crashes go unnoticed, and closing everything leaves orphaned processes holding ports.

## Quick start

```
git clone https://github.com/Mahalakshmiku2003/devrun.git
cd devrun
cmake -S . -B build && cmake --build build
```

Create `devrun.conf` in your project:

```
[db]
cmd = docker run --rm -p 5432:5432 -e POSTGRES_PASSWORD=pw postgres
ready = port 5432

[api]
cmd = cd backend && npm start
needs = db
ready = port 3000

[frontend]
cmd = cd frontend && npm run dev
needs = api
```

Then:

```
devrun up            # foreground, Ctrl+C stops everything
devrun up -d         # background
devrun status        # state, pid and restart count per service
devrun logs api      # recent output of one service
devrun down          # stop everything
```

Requires Linux (or WSL on Windows). It uses `fork`, process groups, `poll` and Unix domain sockets, so it does not run on native Windows.

## Features

- **Dependency ordering:** `needs = db, cache`; services start in topological order. Unknown names and cycles are rejected before anything starts.
- **Health checks:** `ready = port N`; dependents wait until the port accepts connections, with a 15 s timeout.
- **Merged logs:** one colored, timestamped stream, with each service labeled. Partial lines are buffered until complete.
- **Crash recovery:** non-zero exits restart with exponential backoff (1, 2, 4, 8, 16 s), up to 5 attempts. The counter resets after 60 s of stable running.
- **Clean shutdown:** SIGTERM to each service's whole process group, then SIGKILL after 3 s. No orphaned processes.
- **Background mode:** `up -d` double-forks into a daemon. `status`, `logs` and `down` talk to it over a Unix domain socket.

## How it works

A single-threaded event loop built on `poll()` handles everything:

- Each service runs under `sh -c` in its own process group, with stdout and stderr redirected into a pipe.
- One `poll()` call watches every service pipe plus the control socket. Its timeout is set to the next restart or health-check deadline, so no threads or sleeps are needed.
- A signal handler only sets a flag; the loop performs the shutdown. `devrun down` sets the same flag, so Ctrl+C and `down` behave identically.
- A pipe closing (read returns 0) signals that a service exited, and `waitpid` then collects its status, so no zombies are left.

```
src/
  main.cpp      event loop, CLI, daemonizing, shutdown
  process.cpp   fork / setpgid / dup2 / exec
  config.cpp    parser, validation, topological sort
  health.cpp    TCP readiness probe
  ipc.cpp       Unix domain socket client and server
tests/
  run_tests.sh  end-to-end tests against fake services
```

## Tests

```
tests/run_tests.sh build/devrun
```

The tests start the real binary with small configs and check ordering, config errors, backoff timing, health-check gating, background mode, log retrieval and that no processes survive `down`. They also run in CI on every push.

## Limitations

- Linux only; health checks are TCP on localhost only.
- A client that connects to the control socket and sends nothing can stall the loop for up to 0.5 s.
- `logs` returns a snapshot of the last 500 lines per service; it does not follow live.
- Services cannot read keyboard input.

## Related projects

[process-compose](https://github.com/F1bonacc1/process-compose), [Overmind](https://github.com/DarthSim/overmind), [foreman](https://github.com/ddollar/foreman) and Docker Compose solve similar problems. devrun is a smaller, dependency-free C++ take, built to understand process supervision from the system calls up.
