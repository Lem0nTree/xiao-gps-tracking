package com.example.xiaogpstracker

/** The motion state is distinct from receiver standby and from the saved Interval timer. */
internal fun SmartInfo.statusHeadline(): String = when {
    mode != WakeMode.SMART -> "Interval tracking"
    runtimeIntervalFallback -> "Sensor fault · Interval fallback"
    mpuFault || !mpuPresent -> "Motion sensor unavailable"
    else -> when (motionState) {
        SmartMotionState.DISABLED -> "Smart Motion disabled"
        SmartMotionState.ARMED -> "Armed · waiting for motion"
        SmartMotionState.VERIFYING -> "Confirming movement"
        SmartMotionState.ACQUIRING -> "Acquiring GPS fix"
        SmartMotionState.TRACKING -> "Checking movement"
        SmartMotionState.COOLDOWN -> "Motion cooldown"
        SmartMotionState.UNKNOWN -> "Motion status unavailable"
    }
}
