# Multi-stage build for webfsd
FROM alpine:3.19 AS builder

# Install build dependencies
RUN apk add --no-cache \
    build-base \
    make \
    openssl-dev \
    linux-headers

# Create working directory
WORKDIR /src

# Copy source code
COPY . .

# Build webfsd
RUN make clean && make

# Test the binary works
RUN ./webfsd -h

# Runtime stage
FROM alpine:3.19

# Install runtime dependencies
RUN apk add --no-cache \
    openssl \
    ca-certificates \
    && addgroup -g 1000 webfsd \
    && adduser -D -u 1000 -G webfsd webfsd

# Copy the binary from builder stage
COPY --from=builder /src/webfsd /usr/local/bin/webfsd

# Create directory for web content
RUN mkdir -p /var/www && chown webfsd:webfsd /var/www

# Create a simple index.html for testing
RUN echo '<html><body><h1>webfsd is running!</h1><p>Lightweight HTTP server</p></body></html>' > /var/www/index.html \
    && chown webfsd:webfsd /var/www/index.html

# Switch to non-root user
USER webfsd

# Set working directory to web content
WORKDIR /var/www

# Expose port 8000
EXPOSE 8000

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget --no-verbose --tries=1 --spider http://localhost:8000/ || exit 1

# Default command
CMD ["webfsd", "-p", "8000", "-F"]