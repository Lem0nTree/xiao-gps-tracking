package com.example.xiaogpstracker

import org.junit.Assert.assertEquals
import org.junit.Test

class TrackerPresentationTest {
    private val armed = SmartInfo(
        protocolVersion = 2,
        mode = WakeMode.SMART,
        sensitivity = SmartSensitivity.BALANCED,
        motionState = SmartMotionState.ARMED,
        confirmationSeconds = 5,
        standbySliceSeconds = 0,
        fixCooldownSeconds = 120,
        cooldownRemainingSeconds = 0,
        flags = Protocol.SMART_FLAG_MPU_PRESENT or Protocol.SMART_FLAG_MPU_INTERRUPT_ARMED,
        lastWakeReason = 0
    )

    @Test fun headlineReflectsMotionNotReceiverPower() {
        assertEquals("Armed · waiting for motion", armed.statusHeadline())
        assertEquals(armed.statusHeadline(), armed.copy(
            flags = armed.flags or Protocol.SMART_FLAG_CAS12_ACTIVE or
                Protocol.SMART_FLAG_CAS12_VERIFIED
        ).statusHeadline())
        assertEquals(armed.statusHeadline(), armed.copy(
            flags = armed.flags or Protocol.SMART_FLAG_GPS_RECEIVER_ACTIVE
        ).statusHeadline())
    }

    @Test fun motionStatesAreDistinct() {
        val titles = mapOf(
            SmartMotionState.DISABLED to "Smart Motion disabled",
            SmartMotionState.VERIFYING to "Confirming movement",
            SmartMotionState.ACQUIRING to "Acquiring GPS fix",
            SmartMotionState.TRACKING to "Checking movement",
            SmartMotionState.COOLDOWN to "Motion cooldown",
            SmartMotionState.UNKNOWN to "Motion status unavailable"
        )
        titles.forEach { (state, title) ->
            assertEquals(title, armed.copy(motionState = state).statusHeadline())
        }
    }

    @Test fun fallbackTakesPriorityOverSavedMotionState() {
        assertEquals("Sensor fault · Interval fallback", armed.copy(
            flags = Protocol.SMART_FLAG_RUNTIME_INTERVAL_FALLBACK
        ).statusHeadline())
    }

    @Test fun sensorFaultIsVisible() {
        assertEquals("Motion sensor unavailable", armed.copy(flags = 0).statusHeadline())
        assertEquals("Motion sensor unavailable", armed.copy(
            flags = Protocol.SMART_FLAG_MPU_PRESENT
        ).statusHeadline())
    }

    @Test fun intervalDoesNotRequireMotionSensor() {
        assertEquals("Interval tracking", armed.copy(
            mode = WakeMode.INTERVAL, flags = 0
        ).statusHeadline())
    }
}
