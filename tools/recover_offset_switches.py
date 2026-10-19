"""Recover bounded static PPC halfword/byte-offset switches for offline AOT."""
from __future__ import annotations

import struct


def recover(image: bytes, image_base: int, code_ranges=None, *, detailed=False) -> list[dict]:
    if code_ranges is None:
        pe = struct.unpack_from("<I", image, 0x3c)[0]
        count, optional_size = struct.unpack_from("<H", image, pe + 6)[0], struct.unpack_from("<H", image, pe + 20)[0]
        section_table = pe + 24 + optional_size
        code_ranges = []
        for index in range(count):
            offset = section_table + index * 40
            size, address = struct.unpack_from("<II", image, offset + 8)
            flags = struct.unpack_from("<I", image, offset + 36)[0]
            if flags & 0x20000000:
                code_ranges.append((image_base + address, size))
    result = []
    def word(offset):
        return struct.unpack_from(">I", image, offset)[0]
    def signed_low(value):
        low = value & 65535
        return low - 65536 if low & 32768 else low
    def executable(address):
        return not address & 3 and any(start <= address < start + size for start, size in code_ranges)
    def preserves_selector_and_condition(instruction, register):
        opcode = instruction >> 26
        destination = (instruction >> 21) & 31
        if opcode in (14, 15, 32, 34, 40, 42) and destination != register:
            return True  # addi/addis and non-updating integer loads
        if opcode in (36, 38, 44, 50):
            return True  # non-updating stores or lfd (a separate FP register)
        if opcode == 21 and not instruction & 1 and (instruction >> 16) & 31 != register:
            return True  # non-recording rlwinm
        if (opcode == 31 and not instruction & 1 and destination != register and
                (instruction >> 1) & 1023 in (40, 87, 266, 343)):
            return True  # non-recording subf/add, non-updating lbzx/lhax
        return instruction == 0x60000000
    def extent_guard(instruction):
        condition = ((instruction >> 16) & 31) // 4
        relative = instruction >> 26 == 16 and not instruction & 3
        returns = instruction >> 26 == 19 and instruction & 65535 == 0x20
        if ((relative or returns) and (instruction >> 21) & 31 == 12 and
                (instruction >> 16) & 31 == condition * 4 + 1):
            return condition
        return None
    def scheduled_bound(guard_offset, register, condition, begin):
        # The original parser schedules loads/stores and address arithmetic
        # between its unsigned comparison and branch. Walk only instructions
        # proven to preserve both the bounded register and condition field.
        for distance in range(1, 10):
            position = guard_offset - distance * 4
            if position < begin:
                break
            instruction = word(position)
            opcode = instruction >> 26
            if opcode == 10:
                if (not instruction & 0x00200000 and (instruction >> 16) & 31 == register and
                        (instruction >> 23) & 7 == condition):
                    return instruction
                break
            if preserves_selector_and_condition(instruction, register):
                continue
            break
        return None
    for start, size in code_ranges:
        begin, end = start - image_base, start - image_base + size
        if begin < 0 or end > len(image):
            raise ValueError("Code section exceeds original image")
        for offset in range(begin + 8, end - 40, 4):
            if word(offset) >> 16 != 0x3d80:
                continue
            index, adjust = word(offset + 4), word(offset + 8)
            if (adjust >> 16 == 0x398c and index & 0xfc1fffff == 0x5400083c and
                word(offset + 12) == 0x7c0c022e and word(offset + 16) >> 16 == 0x3d80 and
                word(offset + 20) >> 16 == 0x398c):
                register = (index >> 21) & 31
                entry_bytes, scale = 2, 1
                code_high, code_low = word(offset + 16), word(offset + 20)
                tail = offset + 24
                if word(tail) == 0x60000000:
                    tail += 4
            elif index >> 16 == 0x398c and adjust & 0xffff07ff == 0x7c0c00ae:
                # Original CRT/parser variants use byte offsets either
                # directly or scaled by four before adding their code base.
                register = (adjust >> 11) & 31
                adjust = index
                entry_bytes = 1
                high_offset = offset + 12
                scale = 1
                if word(high_offset) == 0x5400103a:
                    scale = 4
                    high_offset += 4
                code_high = word(high_offset)
                if code_high >> 16 != 0x3d80:
                    continue
                low_offset = high_offset + 4
                if word(low_offset) == 0x60000000:
                    low_offset += 4
                code_low = word(low_offset)
                if code_low >> 16 != 0x398c:
                    continue
                tail = low_offset + 4
                if word(tail) == 0x60000000:
                    tail += 4
            else:
                continue
            if [word(tail + index * 4) for index in range(3)] != [0x7d8c0214, 0x7d8903a6, 0x4e800420]:
                continue
            if register in (0, 12):
                # Keep unproven patterns at the existing explicit runtime
                # boundary; never infer a table length from nearby data.
                continue
            # Out-of-range selectors branch or return. Only unchanged
            # selectors/CR fields may be scheduled after that guard.
            condition = None
            guard_offset = offset - 4
            valid_branch, default_branch = word(offset - 8), word(offset - 4)
            valid_condition = ((valid_branch >> 16) & 31) // 4
            # An equivalent original form branches <= bound to setup and
            # sends the greater-than fallthrough to a separate default body.
            # Require the exact +8 skip, an unlinked relative default branch,
            # and a default target outside the dispatch instruction region.
            displacement = default_branch & 0x03fffffc
            if displacement & 0x02000000:
                displacement -= 0x04000000
            default_target = image_base + offset - 4 + displacement
            if (valid_branch >> 26 == 16 and (valid_branch >> 21) & 31 == 4 and
                    (valid_branch >> 16) & 31 == valid_condition * 4 + 1 and
                    valid_branch & 65535 == 8 and default_branch >> 26 == 18 and
                    not default_branch & 3 and
                    not image_base + offset <= default_target < image_base + tail + 12):
                condition = valid_condition
                guard_offset = offset - 8
            for distance in range(9):
                if condition is not None:
                    break
                guard_offset = offset - 4 - distance * 4
                if guard_offset < begin:
                    break
                guard = word(guard_offset)
                condition = extent_guard(guard)
                if condition is not None or not preserves_selector_and_condition(guard, register):
                    break
            if condition is None:
                continue
            compare = scheduled_bound(guard_offset, register, condition, begin)
            if compare is None:
                continue
            count = (compare & 65535) + 1
            table = ((word(offset) & 65535) << 16) + signed_low(adjust)
            code = ((code_high & 65535) << 16) + signed_low(code_low)
            position = table - image_base
            if count > 4096 or position < 0 or count * entry_bytes > len(image) - position:
                raise ValueError("Offset switch table exceeds its original image")
            values = struct.unpack_from(f">{count}{'H' if entry_bytes == 2 else 'B'}", image, position)
            labels = [code + scale * value for value in values]
            if not all(executable(address) for address in labels):
                raise ValueError("Offset switch case points outside original executable sections")
            entry = {"base": image_base + tail + 8, "r": register, "labels": labels}
            if detailed:
                entry.update(table=table, entry_bytes=entry_bytes, scale=scale, code_base=code)
            result.append(entry)
    return result
