Test-only TLS material for the quark::net tests: a self-signed ECDSA P-256 certificate for
`localhost` / `127.0.0.1` (valid 100 years) and its private key. Never use it outside tests.

Regenerate with:

    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout localhost.key \
        -out localhost.crt -days 36500 -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
