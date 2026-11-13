module counters;

// one atomic per wave for the stats of the self-test passes (motion_check, rt_check)

public void waveCount(inout uint counter, bool predicate)
{
    const uint count = WaveActiveCountBits(predicate);
    if (WaveIsFirstLane() && count > 0)
    {
        InterlockedAdd(counter, count);
    }
}

// non-negative floats order like their bits
public void waveMax(inout uint counterBits, float value)
{
    const float wave = WaveActiveMax(value);
    if (WaveIsFirstLane() && wave > 0.0)
    {
        InterlockedMax(counterBits, asuint(wave));
    }
}
