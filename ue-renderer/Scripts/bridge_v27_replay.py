"""Replay logical frames on the Windows shared-memory and loopback transport."""

import ctypes
import mmap
import select
import socket
import struct
import time
import uuid


MAGIC = 0x4D435545
VERSION = 28
HEADER = 64
PAGE = 16 * 1024
BULK_PAGES = 1024
NEAR_PAGES = 256
MAX_SECTION = 44 + 262144 * (101 + 2 * 17)
MAX_LIGHTS = 18 + 4096 * 7
MAX_MATERIAL_LIGHTS = MAX_LIGHTS + 4096 * (5 + 32 * (23 + 24))
LARGE_PAGES = (MAX_SECTION + MAX_LIGHTS + PAGE - 1) // PAGE
TERRAIN_BYTES = HEADER + PAGE * (BULK_PAGES + NEAR_PAGES + LARGE_PAGES)


def frame(kind, body=b""):
    return struct.pack(">IB", len(body) + 1, kind) + body


def frames(payload):
    offset = 0
    while offset < len(payload):
        size, kind = struct.unpack_from(">IB", payload, offset)
        end = offset + 4 + size
        assert 1 <= size and end <= len(payload)
        yield kind, bytes(payload[offset + 5:end])
        offset = end


class BridgeReplay:
    def __init__(self, port, *, timeout=15, version=VERSION):
        self.port = port
        self.timeout = timeout
        self.version = version
        self.terrain_leases = {}
        self.release_order = []
        self.incoming = bytearray()
        self.meshes = {}
        self.lights = {}
        self.scene = self.token = self.camera_seq = self.ready_scene = 0

    def __enter__(self):
        control_id, terrain_id, session_id = uuid.uuid4(), uuid.uuid4(), uuid.uuid4()
        self.control = self.terrain = self.socket = None
        try:
            self.control = mmap.mmap(-1, 640, tagname="{" + str(control_id).upper() + "}")
            self.terrain = mmap.mmap(-1, TERRAIN_BYTES, tagname="{" + str(terrain_id).upper() + "}")
            nonce = session_id.int & (2**64 - 1)
            struct.pack_into("<IIQ", self.control, 0, MAGIC, self.version, nonce)
            struct.pack_into("<IIIIQ", self.terrain, 0, MAGIC, self.version, TERRAIN_BYTES, HEADER, nonce)
            self.socket = socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout)
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            reset = frame(1, struct.pack(">IIQ", MAGIC, self.version, nonce))
            self.socket.sendall(reset + frame(21, control_id.bytes + terrain_id.bytes))
            received = b""
            while len(received) < len(reset):
                chunk = self.socket.recv(len(reset) - len(received))
                if not chunk:
                    raise ConnectionError(f"UE disconnected before acknowledging the v{self.version} mappings")
                received += chunk
            if received != reset:
                raise RuntimeError(f"Invalid v{self.version} reset confirmation: {received.hex()}")
            self.socket.sendall(frame(12, struct.pack(">iiHHB", 0, 0, 854, 480, 0)))
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, _type, _value, _traceback):
        for resource in (self.socket, self.terrain, self.control):
            if resource is not None:
                resource.close()

    def receive(self, timeout=0):
        if not select.select([self.socket], [], [], timeout)[0]:
            return
        chunk = self.socket.recv(65536)
        if not chunk:
            raise ConnectionError("UE disconnected during replay")
        self.incoming.extend(chunk)
        offset = 0
        while len(self.incoming) - offset >= 5:
            size, kind = struct.unpack_from(">IB", self.incoming, offset)
            assert 1 <= size <= 32 * 1024
            end = offset + 4 + size
            if end > len(self.incoming):
                break
            if kind == 8:
                payload = offset + 5
                self.ready_scene, = struct.unpack_from(">I", self.incoming, payload + 16)
                event_count = self.incoming[payload + 21]
                release_count = self.incoming[payload + 63]
                assert size == 65 + event_count * 6 + release_count * 8
                for release in range(release_count):
                    token, = struct.unpack_from(">Q", self.incoming, payload + 64 + event_count * 6 + release * 8)
                    self.terrain_leases.pop(token)
                    self.release_order.append(token)
            offset = end
        del self.incoming[:offset]

    def wait_until(self, predicate, *, timeout=30):
        end = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() > end:
                raise TimeoutError(f"UE replay stalled (ready={self.ready_scene}, leases={len(self.terrain_leases)})")
            self.receive(0.05)

    def publish_camera(self, pose, capture=0, movement_flags=0):
        kind, body = next(frames(pose))
        assert kind == 5 and len(body) == 89
        words = struct.unpack("<12Q", body + struct.pack(">IB", capture, movement_flags) + bytes(2))
        ctypes.c_uint64.from_buffer(self.control, 448).value = self.camera_seq + 1
        for index, word in enumerate(words):
            ctypes.c_uint64.from_buffer(self.control, 512 + index * 8).value = word
        self.camera_seq += 2
        ctypes.c_uint64.from_buffer(self.control, 448).value = self.camera_seq

    def publish_teleport_epoch(self, epoch):
        # MC state sequence at 64, body at 128, teleport epoch at body + 32.
        sequence = ctypes.c_uint64.from_buffer(self.control, 64)
        world_and_flags = ctypes.c_uint64.from_buffer(self.control, 128)
        teleport_and_hit_x = ctypes.c_uint64.from_buffer(self.control, 160)
        sequence.value += 1
        world_and_flags.value = (world_and_flags.value & 0xFFFFFFFF00000000) | self.scene
        teleport_and_hit_x.value = (teleport_and_hit_x.value & 0xFFFFFFFF00000000) | epoch
        sequence.value += 1

    def _claim(self, pages, urgent):
        if pages > BULK_PAGES:
            lanes = ((BULK_PAGES + NEAR_PAGES, LARGE_PAGES, 1),)
        elif urgent:
            lanes = ((BULK_PAGES, NEAR_PAGES, 32), (0, BULK_PAGES, 96))
        else:
            lanes = ((0, BULK_PAGES, 96),)
        for start, capacity, max_leases in lanes:
            used = sorted((page, count) for page, count in self.terrain_leases.values() if start <= page < start + capacity)
            if len(used) >= max_leases:
                continue
            page = start
            for occupied, count in used:
                if page + pages <= occupied:
                    break
                page = occupied + count
            if page + pages <= start + capacity:
                return page
        return None

    def section(self, mesh, lights, urgent=False):
        """Reserve and write a section, returning None if no credit or the descriptor bytes."""
        assert 44 <= len(mesh) <= MAX_SECTION and 18 <= len(lights) <= MAX_MATERIAL_LIGHTS
        assert len(mesh) + len(lights) <= MAX_SECTION + MAX_LIGHTS
        assert lights[:16] == struct.pack(">I", self.scene) + mesh[:12]
        pages = (len(mesh) + len(lights) + PAGE - 1) // PAGE
        page = self._claim(pages, urgent)
        if page is None:
            return None
        self.token += 1
        self.terrain_leases[self.token] = (page, pages)
        offset = HEADER + PAGE * page
        self.terrain[offset:offset + len(mesh)] = mesh
        end = offset + len(mesh)
        self.terrain[end:end + len(lights)] = lights
        return frame(38, struct.pack(">IQIIIB", self.scene, self.token, offset, len(mesh), len(lights), int(urgent)))

    def sendall(self, payload):
        """Wrap a batch of existing v26 logical frames, pairing mesh and light bodies by coordinate."""
        outgoing = bytearray()
        entries = list(frames(payload))
        index = 0
        while index < len(entries):
            kind, body = entries[index]
            index += 1
            if kind == 1:
                continue
            if kind == 19 and self.version >= 28:
                continue  # Legacy block debris is now carried by DynamicMesh and InstanceBatch.
            if kind == 5:
                self.publish_camera(frame(kind, body))
                continue
            if kind == 11:
                self.scene, = struct.unpack_from(">I", body)
                self.meshes.clear()
                self.lights.clear()
            elif kind == 3:
                columns = set(struct.iter_unpack(">ii", body))
                for key in list(self.meshes):
                    x, _, z = struct.unpack(">iii", key)
                    if (x, z) in columns:
                        self.meshes.pop(key)
                        self.lights.pop(key, None)
            if kind not in (2, 25):
                outgoing.extend(frame(kind, body))
                continue
            if kind == 2:
                self.meshes[body[:12]] = body
                mesh = body
                if index < len(entries) and entries[index][0] == 25 and entries[index][1][4:16] == mesh[:12]:
                    self.lights[mesh[:12]] = entries[index][1]
                    index += 1
                lights = self.lights.get(mesh[:12], struct.pack(">I", self.scene) + mesh[:12] + bytes(2))
            else:
                self.lights[body[4:16]] = body
                mesh, lights = self.meshes[body[4:16]], body
            descriptor = self.section(mesh, lights)
            if descriptor is None:
                self.socket.sendall(outgoing)
                outgoing.clear()
                deadline = time.monotonic() + self.timeout
                while descriptor is None:
                    if time.monotonic() > deadline:
                        raise TimeoutError("UE did not return terrain space")
                    self.receive(0.05)
                    descriptor = self.section(mesh, lights)
            outgoing.extend(descriptor)
        if outgoing:
            self.socket.sendall(outgoing)
        self.receive()
