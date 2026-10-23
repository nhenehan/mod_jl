# How it works

You don't need this chapter to write applications. It's here for the
curious, and for when you're reading a packet capture.

## One request, start to finish

The worker protocol, **jlp**, follows the shape of AJP, the protocol between
Apache's mod_jk and Tomcat:

```text
mod_jl                                   worker
  │── FORWARD_REQUEST (head + first body block) ──▶│  handler starts
  │◀──────────────────────────── GET_BODY_CHUNK ───│  handler reads past the first block
  │── BODY_CHUNK ─────────────────────────────────▶│
  │◀────────────────────────────── SEND_HEADERS ───│  status and headers
  │◀──────────────────────────────── BODY_CHUNK ───│  response body, 0 or more
  │◀────────────────────────────── END_RESPONSE ───│  "reuse this connection: yes/no"
```

- The connection carries **one request at a time** and is reused for the
  next one. Each exchange (a request, or a `CPING`) gets a new request id,
  which every frame of it carries, so a frame left over from an earlier
  exchange is caught rather than taken for an answer.
- The **request body is pulled**: the worker asks for each block when the
  handler reads it. That's why an unread upload costs almost nothing.
- **`CPING`/`CPONG`** check that a worker is alive. **`SHUTDOWN`** tells the
  client an idle connection is going away. **`ERROR`** reports a malformed
  request.

## Why EXI

AJP, mod_jk's protocol, is a binary packet format of its own. jlp keeps
AJP's shape but writes each message as an XML document, described by a
schema (`schema/jlp2.xsd`, "HTTP as XML"), and encoded as **EXI**, the W3C's
binary XML format, in its strict, schema-informed form:

- **Compact.** The schema tells both ends what can come next, so names of
  elements and attributes are never sent, a method or one of about a
  hundred common header names is a small number, and repeated strings are
  sent once. A request head comes out smaller than its HTTP/1.1 text.
- **Checked.** A message that doesn't fit the schema can't be decoded, on
  either side. Anything unusual still fits: unknown methods and headers
  have their own elements, and header bytes that aren't valid UTF-8 travel
  as raw bytes.
- **Two codecs, one schema.** The Apache side uses rexi (Rust) and the
  workers use jexi (Julia), each built from the same XSD. Every message is
  a round trip between two independent EXI implementations, which is half
  the fun of this project; the test suite checks that both produce the same
  bytes.

The byte-aligned flavour of EXI is used so that a body chunk is a short
prefix followed by the bytes themselves, which both sides send and read
in place.

## Frames

Every message is a frame: a 12-byte header and a payload.

```text
magic 0x4A │ version 0x02 │ type │ flags │ request id (u32) │ payload length (u32)
```

Each connection starts with **HELLO** and **HELLO_ACK**. They exchange:

- the hash of the message schema, so that two builds that disagree about the
  format refuse each other instead of misreading each other;
- the largest frame each side accepts;
- the shared secret.

## HTTP as XML, sent as EXI

Each payload after the handshake is one document of a small XML vocabulary
for HTTP: a request has a method, a target, headers, and so on. The
vocabulary is in `schema/jlp2.xsd`.

The documents aren't sent as XML text. They're sent as **EXI** (Efficient
XML Interchange), a W3C binary encoding that, given the schema, turns them
into compact, quick-to-parse bytes:

- the 81 well-known header names and 30 methods become small numbers;
- other strings are sent once and then referred to.

The sample request in `schema/samples/forward_get.xml` is 143 bytes as EXI.
Its HTTP/1.1 text is 145 bytes, and the EXI version also carries what Apache
knows about the connection: client and server addresses, ports, the route.

Body blocks are the exception to the encoding cost. Each one has a fixed
three-byte prefix and a length, and then the body bytes as they are, so
neither side runs a codec over body data.
