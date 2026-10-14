#include "lemma/units.h"

#include <numbers>

namespace lemma {

namespace {

const Dimension kLength = Dimension::of(1);
const Dimension kMass = Dimension::of(0, 1);
const Dimension kTime = Dimension::of(0, 0, 1);
const Dimension kCurrent = Dimension::of(0, 0, 0, 1);
const Dimension kTemperature = Dimension::of(0, 0, 0, 0, 1);
const Dimension kAmount = Dimension::of(0, 0, 0, 0, 0, 1);
const Dimension kLuminosity = Dimension::of(0, 0, 0, 0, 0, 0, 1);
const Dimension kNone;

const Dimension kArea = kLength * kLength;
const Dimension kVolume = kArea * kLength;
const Dimension kForce = kMass * kLength / (kTime * kTime);
const Dimension kEnergy = kForce * kLength;
const Dimension kPower = kEnergy / kTime;
const Dimension kPressure = kForce / kArea;
const Dimension kCharge = kCurrent * kTime;
const Dimension kVoltage = kPower / kCurrent;
const Dimension kResistance = kVoltage / kCurrent;
const Dimension kFrequency = kNone / kTime;

constexpr double kLbf = 4.4482216152605;
constexpr double kPsi = 6894.757293168361;

} // namespace

UnitRegistry::UnitRegistry()
{
    const auto add = [this](std::string name, double factor, const Dimension& dim) {
        units_.emplace(name, Unit{name, {factor, dim}});
    };

    // Length
    add("m", 1.0, kLength);
    add("km", 1e3, kLength);
    add("cm", 1e-2, kLength);
    add("mm", 1e-3, kLength);
    add("μm", 1e-6, kLength);
    add("in", 0.0254, kLength);
    add("ft", 0.3048, kLength);
    add("yd", 0.9144, kLength);
    add("mi", 1609.344, kLength);

    // Mass
    add("kg", 1.0, kMass);
    add("gm", 1e-3, kMass);
    add("mg", 1e-6, kMass);
    add("tonne", 1e3, kMass);
    add("lb", 0.45359237, kMass);
    add("slug", kLbf / 0.3048, kMass);

    // Time
    add("s", 1.0, kTime);
    add("ms", 1e-3, kTime);
    add("min", 60.0, kTime);
    add("hr", 3600.0, kTime);
    add("day", 86400.0, kTime);

    // Other SI base units
    add("A", 1.0, kCurrent);
    add("mA", 1e-3, kCurrent);
    add("K", 1.0, kTemperature);
    add("R", 5.0 / 9.0, kTemperature);
    add("mol", 1.0, kAmount);
    add("cd", 1.0, kLuminosity);

    // Area and volume
    add("L", 1e-3, kVolume);
    add("mL", 1e-6, kVolume);
    add("gal", 3.785411784e-3, kVolume);

    // Force
    add("N", 1.0, kForce);
    add("kN", 1e3, kForce);
    add("MN", 1e6, kForce);
    add("lbf", kLbf, kForce);
    add("kip", 1e3 * kLbf, kForce);

    // Energy and power
    add("J", 1.0, kEnergy);
    add("kJ", 1e3, kEnergy);
    add("MJ", 1e6, kEnergy);
    add("cal", 4.184, kEnergy);
    add("kcal", 4184.0, kEnergy);
    add("BTU", 1055.05585262, kEnergy);
    add("kWh", 3.6e6, kEnergy);
    add("W", 1.0, kPower);
    add("kW", 1e3, kPower);
    add("MW", 1e6, kPower);
    add("hp", 745.69987158227022, kPower);

    // Pressure and stress
    add("Pa", 1.0, kPressure);
    add("kPa", 1e3, kPressure);
    add("MPa", 1e6, kPressure);
    add("GPa", 1e9, kPressure);
    add("psi", kPsi, kPressure);
    add("ksi", 1e3 * kPsi, kPressure);
    add("bar", 1e5, kPressure);
    add("atm", 101325.0, kPressure);

    // Electrical
    add("C", 1.0, kCharge);
    add("V", 1.0, kVoltage);
    add("kV", 1e3, kVoltage);
    add("Ω", 1.0, kResistance);
    add("ohm", 1.0, kResistance);

    // Frequency and angle (angles are dimensionless, measured in radians)
    add("Hz", 1.0, kFrequency);
    add("rad", 1.0, kNone);
    add("deg", std::numbers::pi / 180.0, kNone);

    preferred_ = {"m", "kg", "s", "A", "K", "mol", "cd", "N", "J", "W", "Pa", "C", "V", "Ω"};
}

const UnitRegistry& UnitRegistry::instance()
{
    static const UnitRegistry registry;
    return registry;
}

const Unit* UnitRegistry::find(std::string_view name) const
{
    const auto it = units_.find(std::string(name));
    return it == units_.end() ? nullptr : &it->second;
}

const Unit* UnitRegistry::preferredFor(const Dimension& dim) const
{
    for (const std::string& name : preferred_) {
        const Unit* unit = find(name);
        if (unit->value.dim == dim) {
            return unit;
        }
    }
    return nullptr;
}

} // namespace lemma
