package com.example.xiaogpstracker

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant
import java.util.Locale

data class GpsRecord(
    val seq: Long,
    val epoch: Long,
    val latE7: Int,
    val lonE7: Int,
    val hdopX100: Int,
    val satellites: Int
) {
    val latitude: Double get() = latE7 / 10_000_000.0
    val longitude: Double get() = lonE7 / 10_000_000.0
    val hdop: Double get() = hdopX100 / 100.0

    fun utcIso(): String = Instant.ofEpochSecond(epoch).toString()

    fun toDeviceBytes(): ByteArray {
        val first19 = ByteBuffer.allocate(19)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(seq.toInt())
            .putInt(epoch.toInt())
            .putInt(latE7)
            .putInt(lonE7)
            .putShort(hdopX100.toShort())
            .put(satellites.toByte())
            .array()

        return first19 + byteArrayOf(crc8(first19))
    }

    companion object {
        const val SIZE = 20

        fun fromDeviceBytes(bytes: ByteArray, offset: Int = 0): GpsRecord? {
            if (offset < 0 || offset + SIZE > bytes.size) return null

            val raw = bytes.copyOfRange(offset, offset + SIZE)
            val expectedCrc = crc8(raw.copyOfRange(0, 19))
            if (raw[19] != expectedCrc) return null

            val b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
            val seq = b.int.toLong() and 0xFFFF_FFFFL
            val epoch = b.int.toLong() and 0xFFFF_FFFFL
            val lat = b.int
            val lon = b.int
            val hdop = b.short.toInt() and 0xFFFF
            val sats = b.get().toInt() and 0xFF

            if (seq == 0L || seq == 0xFFFF_FFFFL) return null

            return GpsRecord(seq, epoch, lat, lon, hdop, sats)
        }

        private fun crc8(data: ByteArray): Byte {
            var crc = 0
            for (value in data) {
                crc = crc xor (value.toInt() and 0xFF)
                repeat(8) {
                    crc = if ((crc and 0x80) != 0) {
                        ((crc shl 1) xor 0x07) and 0xFF
                    } else {
                        (crc shl 1) and 0xFF
                    }
                }
            }
            return crc.toByte()
        }
    }
}
