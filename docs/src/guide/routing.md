# Routing

A `Router` maps a method and a path pattern to a function.

```julia
app = Router()

route!(app, "GET", "/users/{id}") do req, params
    "user $(params[:id])\n"
end
```

There are two kinds of route:

| Register with | The function gets | Use it for |
|---|---|---|
| `route!(f, app, method, pattern)` | `(req::Request, params)` and returns a response | almost everything |
| `stream!(f, app, method, pattern)` | `(s::Stream, params)` and writes the response itself | large or open-ended responses, reading and writing at the same time ([Streaming](streaming.md)) |

## Patterns

Patterns are matched segment by segment against the request path:

| Pattern | Matches | `params` |
|---|---|---|
| `/about` | `/about` | (empty) |
| `/users/{id}` | `/users/42` | `params[:id] == "42"` |
| `/users/{id}/posts/{post}` | `/users/42/posts/7` | `:id`, `:post` |
| `/files/*` | `/files/a/b/c.txt` | `params[:*] == "a/b/c.txt"` |

- `{name}` captures exactly one segment.
- A trailing `*` matches the rest of the path, however many segments, and
  captures it as `params[:*]`.
- Empty segments are ignored, so `/users//42/` matches `/users/{id}`.
- Captured values are **still percent-encoded**, exactly as the client sent
  them: `/users/a%20b` gives `"a%20b"`. Validate them before using them as
  file names or keys.
- The query string isn't part of the path. Read it from `req.query`.

Routes are tried **in the order you register them**, and the first match
wins. Put specific routes before general ones:

```julia
route!(app, "GET", "/users/me")   do req, p ... end   # first
route!(app, "GET", "/users/{id}") do req, p ... end
```

## Methods

The method is matched exactly (`"GET"`, `"POST"`, `"PUT"`, `"DELETE"`, ...).
`"*"` matches any method. A `GET` route also answers `HEAD`.

## When nothing matches

- If no route matches the path, the answer is `404 Not Found`.
- If a route matches the path but not the method, the answer is
  `405 Method Not Allowed`.

To handle unmatched requests yourself (a custom 404 page, or passing
everything else to another handler), give a fallback. It gets just the
request:

```julia
app = Router(fallback = req -> Response(404, ["content-type" => "text/html"], read("404.html")))
```
