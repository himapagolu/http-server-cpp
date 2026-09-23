# http-server-cpp

A small HTTP/1.1 server written from scratch in C++, using raw POSIX sockets — no libcurl, no Boost.Asio, no frameworks. I built this to get my hands dirty with the stuff that usually hides behind a library: parsing an HTTP request byte by byte, handling multiple clients at once, and figuring out where a naive implementation quietly breaks.

## What it actually does

The server listens on port 4221 and understands a handful of routes:

- `GET /` — just a 200 OK, useful as a health check.
- `GET /echo/{text}` — sends back whatever you put in the path. If the client sends `Accept-Encoding: gzip`, the response body gets compressed with zlib and comes back with `Content-Encoding: gzip`.
- `GET /user-agent` — echoes the `User-Agent` header back at you.
- `GET /files/{name}` — reads a file out of a directory you pass in on startup (`--directory ./somewhere`) and streams it back.
- `POST /files/{name}` — writes the request body to disk as a new file.
- Anything else — 404.

Every connection is handled on its own thread (`std::thread` + `detach`), and connections are kept alive by default so a client can fire off several requests without reconnecting each time — it only closes when the client asks for `Connection: close`.

## How it's put together

- **`HttpRequestParser`** takes the raw bytes off the socket and turns them into a `HttpRequest` struct (method, path, headers, body). It reads `Content-Length` to know how much body to slice off.
- **`HttpResponseBuilder`** is a small fluent builder (`.status().header().body().build()`) so I'm not hand-concatenating strings with `\r\n` everywhere.
- **`Router`** is basically a Strategy pattern — each route is registered with a method, a path-matching rule (exact / prefix / contains), and a handler function. Adding a new endpoint just means adding one line in `buildRouter()`, not touching the dispatch logic.
- Handlers live in the `handlers` namespace and are kept deliberately dumb — one job each.


This started as my solution to the [CodeCrafters "Build Your Own HTTP Server" challenge](https://codecrafters.io/challenges/http-server), so along the way I ended up working through port binding and basic responses, URL/header parsing, concurrent connections, serving and writing files, HTTP compression (gzip, with support for multiple encoding schemes), and persistent (keep-alive) connections — including handling several of them concurrently and closing them cleanly.

## Why this exists

I wanted a project that forced me to actually understand sockets, threading, and the HTTP spec instead of importing something that hides it all. It's rough around the edges in the ways listed above, and that's intentional and it's a working snapshot of a systems-level C++ project I'm actively iterating on, not a finished product.
