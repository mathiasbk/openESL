# openesl-ap in a container (docs/linux-ap.md, "In Docker"). Build from the repository root:
#
#   docker build -f ap/linux/Dockerfile -t openesl-ap .
#   docker run -d --name openesl --restart unless-stopped -p 80:80 -v openesl:/var/lib/openesl \
#       --device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...:/dev/ttyACM0 \
#       openesl-ap --radio uart:/dev/ttyACM0
#
# The program is built statically against musl for the platform the image is built for
# (docker buildx --platform linux/arm64,linux/arm/v7,linux/amd64 works too), so the image holds
# nothing but the binary. The web page's Design tab needs the renderer (web/dist/wasm/, built
# with Emscripten): build it first and it is packed in; without it the AP has the Dashboard tab.
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base python3
WORKDIR /src
COPY . .
RUN make -C ap/linux -j"$(nproc)" LDFLAGS=-static && cp ap/linux/build/host/openesl-ap /openesl-ap

FROM scratch
COPY --from=build /openesl-ap /openesl-ap
VOLUME /var/lib/openesl
# the API and dashboard; 4553 for network radios (--netradio 4553)
EXPOSE 80 4553
ENTRYPOINT ["/openesl-ap"]
CMD ["--radio", "sim"]
