package com.example.xiaogpstracker

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class ProtocolTest {

    @Test
    fun parsesAllSupportedInfoPacketSizes() {
        val info27 = Protocol.parseInfo(infoPayload(27))
        assertNotNull(info27)
        assertEquals(0x11223344L, info27!!.storedCount)
        assertEquals(0x77889900L, info27.newestSeq)
        assertEquals(0x12345678, info27.lastLatE7)
        assertEquals(0x87654321.toInt(), info27.lastLonE7)
        assertEquals(0, info27.firmwareMajor)

        val info55 = Protocol.parseInfo(infoPayload(55))
        assertNotNull(info55)
        assertEquals(0x01020304L, info55!!.gpsCharsProcessed)
        assertEquals(0xAABBCCDDL, info55.gpsSentencesWithFix)
        assertEquals(0x42, info55.gpsFlags)
        assertEquals(0xFFEEDDCCL, info55.gpsLocationAgeMs)

        val info65 = Protocol.parseInfo(infoPayload(65))
        assertNotNull(info65)
        assertEquals(0x07, info65!!.powerFlags)
        assertEquals(900, info65.logIntervalSeconds)
        assertEquals(3300, info65.batteryMillivolts)
        assertEquals("2.0.0", info65.firmwareVersion)

        assertNull(Protocol.parseInfo(ByteArray(26)))
        assertNull(Protocol.parseInfo(ByteArray(66)))
    }

    @Test
    fun parsesVersionedLittleEndianSmartInfoPayloads() {
        val payload = byteArrayOf(
            2, // protocol version
            1, // Smart mode
            1, // Balanced
            5, // Cooldown
            5, // confirmation seconds
            120, // standby slice seconds
            0x34, 0x12, // fix cooldown: 0x1234
            0x78, 0x56, // remaining: 0x5678
            (Protocol.SMART_FLAG_MPU_PRESENT or
                Protocol.SMART_FLAG_MPU_INTERRUPT_ARMED or
                Protocol.SMART_FLAG_CAS12_VERIFIED or
                Protocol.SMART_FLAG_CAS12_ACTIVE or
                Protocol.SMART_FLAG_GPS_RECEIVER_ACTIVE).toByte(),
            9 // last wake reason
        )

        val info = Protocol.parseSmartInfo(payload)
        assertNotNull(info)
        assertEquals(2, info!!.protocolVersion)
        assertEquals(WakeMode.SMART, info.mode)
        assertEquals(SmartSensitivity.BALANCED, info.sensitivity)
        assertEquals(SmartMotionState.COOLDOWN, info.motionState)
        assertEquals(TrackingProfile.CONTINUOUS, info.profile)
        assertEquals(false, info.supportsProfiles)
        assertEquals(5, info.confirmationSeconds)
        assertEquals(120, info.standbySliceSeconds)
        assertEquals(0x1234, info.fixCooldownSeconds)
        assertEquals(0x5678, info.cooldownRemainingSeconds)
        assertEquals(9, info.lastWakeReason)
        assertEquals(true, info.mpuPresent)
        assertEquals(true, info.cas12Active)
        assertEquals(true, info.gpsReceiverActive)
        assertEquals(SmartWakeReason.UNKNOWN, info.wakeReason)

        assertNull(Protocol.parseSmartInfo(ByteArray(11)))
        assertNull(Protocol.parseSmartInfo(payload + byteArrayOf(TrackingProfile.POINT_TO_POINT.wireValue.toByte())))

        val profilePayload = (payload + byteArrayOf(TrackingProfile.POINT_TO_POINT.wireValue.toByte())).also {
            it[0] = Protocol.SMART_INFO_PROTOCOL_V3.toByte()
        }
        val profileInfo = Protocol.parseSmartInfo(profilePayload)
        assertNotNull(profileInfo)
        assertEquals(3, profileInfo!!.protocolVersion)
        assertEquals(TrackingProfile.POINT_TO_POINT, profileInfo.profile)
        assertEquals(true, profileInfo.supportsProfiles)
        assertEquals(SmartMotionState.COOLDOWN, profileInfo.motionState)
        assertNull(Protocol.parseSmartInfo(profilePayload.copyOf(12)))

        val stopPayload = profilePayload.copyOf()
        stopPayload[3] = SmartMotionState.ACQUIRING_STOP.wireValue.toByte()
        stopPayload[11] = SmartWakeReason.STOP.wireValue.toByte()
        val stopInfo = Protocol.parseSmartInfo(stopPayload)
        assertNotNull(stopInfo)
        assertEquals(SmartMotionState.ACQUIRING_STOP, stopInfo!!.motionState)
        assertEquals(SmartWakeReason.STOP, stopInfo.wakeReason)

        val unknownVersion = payload.copyOf()
        unknownVersion[0] = 4
        assertNull(Protocol.parseSmartInfo(unknownVersion))
    }

    @Test
    fun encodesSmartCommandsAndKeepsIntervalPayloadLittleEndian() {
        val getSmart = Protocol.smartInfoRequest()
        assertEquals(Protocol.CMD_GET_SMART_INFO, getSmart[2].toInt() and 0xFF)
        assertArrayEquals(byteArrayOf(), framePayload(getSmart))
        assertArrayEquals(
            byteArrayOf(WakeMode.SMART.wireValue.toByte(), SmartSensitivity.LOW.wireValue.toByte()),
            framePayload(Protocol.setSmartConfigRequest(WakeMode.SMART, SmartSensitivity.LOW))
        )
        assertArrayEquals(
            byteArrayOf(
                WakeMode.SMART.wireValue.toByte(),
                SmartSensitivity.BALANCED.wireValue.toByte(),
                TrackingProfile.POINT_TO_POINT.wireValue.toByte()
            ),
            framePayload(
                Protocol.setSmartConfigRequest(
                    WakeMode.SMART,
                    SmartSensitivity.BALANCED,
                    TrackingProfile.POINT_TO_POINT
                )
            )
        )
        assertArrayEquals(
            byteArrayOf(0x84.toByte(), 0x03, 0x00, 0x00),
            framePayload(Protocol.setIntervalRequest(900))
        )
    }

    @Test
    fun packetParserHandlesFragmentedSmartAck() {
        val frame = Protocol.encode(
            Protocol.RSP_ACK,
            byteArrayOf(Protocol.CMD_SET_SMART_CONFIG.toByte())
        )
        val parser = PacketParser()
        assertEquals(0, parser.feed(frame.copyOfRange(0, 3)).size)
        val packets = parser.feed(frame.copyOfRange(3, frame.size))
        assertEquals(1, packets.size)
        assertEquals(Protocol.RSP_ACK, packets[0].type)
        assertArrayEquals(byteArrayOf(Protocol.CMD_SET_SMART_CONFIG.toByte()), packets[0].payload)
    }

    private fun framePayload(frame: ByteArray): ByteArray =
        frame.copyOfRange(5, frame.size - 2)

    private fun infoPayload(size: Int): ByteArray {
        val b = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(0x11223344)
        b.putInt(0x55667788)
        b.putInt(0x77889900.toInt())
        b.putInt(0x01020304)
        b.putInt(0x12345678)
        b.putInt(0x87654321.toInt())
        b.putShort(250.toShort())
        b.put(12.toByte())

        if (size >= 55) {
            b.putInt(0x01020304)
            b.putInt(0xAABBCCDD.toInt())
            b.putInt(0x10203040)
            b.putInt(0x0A0B0C0D)
            b.putInt(0x0D0C0B0A)
            b.putShort(175.toShort())
            b.put(8.toByte())
            b.put(0x42.toByte())
            b.putInt(0xFFEEDDCC.toInt())
        }

        if (size == 65) {
            b.put(0x07.toByte())
            b.putShort(15.toShort())
            b.putShort(900.toShort())
            b.putShort(3300.toShort())
            b.put(2.toByte())
            b.put(0.toByte())
            b.put(0.toByte())
        }
        return b.array()
    }
}
