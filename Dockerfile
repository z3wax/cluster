# SDrive Cluster — koordinator (bosh PC) serveri.
# Bitta statik binar: veb-panel (go:embed) va API. Ma'lumotlar /data volume'ida.
#
#   docker build -t sdrive-server .
#   docker run -p 8080:8080 -v sdrive-data:/data -e SDRIVE_ADMIN_TOKEN=... sdrive-server

# ---- build ----
FROM golang:1.22-alpine AS build
WORKDIR /src
# bog'liqliklar yo'q (faqat standart kutubxona) — go.mod/go.sum avval nusxalanadi, keshlanadi
COPY backend/go.mod ./backend/
RUN cd backend && go mod download
COPY backend ./backend
RUN cd backend && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/sdrive-server .

# ---- runtime ----
FROM alpine:3.20
RUN apk add --no-cache ca-certificates tzdata && adduser -D -u 10001 sdrive
WORKDIR /app
COPY --from=build /out/sdrive-server /app/sdrive-server
COPY SHARTNOMA.md /app/SHARTNOMA.md
# veb-fayllar binarga joylangan (go:embed), lekin ochiq yangilash uchun ham qo'yamiz
COPY backend/web /app/web

ENV SDRIVE_DATA_DIR=/data
VOLUME /data
RUN mkdir -p /data && chown -R sdrive:sdrive /data /app
USER sdrive

EXPOSE 8080
# Hosting platformalari $PORT beradi; aks holda 8080.
ENTRYPOINT ["/app/sdrive-server"]
CMD ["-web", "/app/web", "-agreement", "/app/SHARTNOMA.md"]
