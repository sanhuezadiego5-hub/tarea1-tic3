#include "wifi_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    imu_wifi_profile_t profile = {true, false, "WIFI_COMPARTIDO", "test-only-password",
                                  "192.0.2.10", 5001};
    assert(imu_wifi_profile_valid(&profile));
    profile.password = "";
    assert(!imu_wifi_profile_valid(&profile));
    profile.open_network = true;
    assert(imu_wifi_profile_valid(&profile));
    profile.password = "not-empty";
    assert(!imu_wifi_profile_valid(&profile));
    profile.open_network = false;
    profile.password = "test-only-password";
    profile.server_host = "raspberrypi.local";
    assert(imu_wifi_profile_valid(&profile));
    profile.server_host = "http://192.0.2.10";
    assert(!imu_wifi_profile_valid(&profile));
    profile.server_host = "";
    assert(!imu_wifi_profile_valid(&profile));
    profile.server_host = "192.0.2.10";
    profile.ssid = "12345678801234567880123456788012";
    assert(imu_wifi_profile_valid(&profile));
    profile.ssid = "12345678801234567880123456788012345";
    assert(!imu_wifi_profile_valid(&profile));
    profile.ssid = "WIFI_COMPARTIDO";
    profile.enabled = false;
    assert(!imu_wifi_profile_valid(&profile));
    assert(!imu_wifi_profile_valid(NULL));

    bool eligible[3] = {true, true, true}, visible[3] = {true, true, true};
    uint64_t cooldown[3] = {0};
    assert(imu_wifi_pick(eligible, visible, cooldown, 0) == 0);
    visible[0] = false;
    assert(imu_wifi_pick(eligible, visible, cooldown, 0) == 1);
    eligible[1] = false; /* Unknown lab SSID, current WiFi is still selectable. */
    assert(imu_wifi_pick(eligible, visible, cooldown, 0) == 2);
    visible[0] = true;
    cooldown[0] = 60000; /* Preferred AP with bad credentials must not starve #3. */
    assert(imu_wifi_pick(eligible, visible, cooldown, 59999) == 2);
    assert(imu_wifi_pick(eligible, visible, cooldown, 60000) == 0);
    visible[0] = visible[2] = false;
    assert(imu_wifi_pick(eligible, visible, cooldown, 60000) == -1);
    for (unsigned i = 0; i < 5; i++) assert(imu_wifi_backoff_ms(i) == 1000u << i);
    assert(imu_wifi_backoff_ms(5) == 30000);
    assert(imu_wifi_backoff_ms(100) == 30000);
    puts("PASS: WiFi priorities, incomplete profiles, fallback cooldown and capped backoff");
}
