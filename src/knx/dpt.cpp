#include "dpt.h"

#include "bits.h"

Dpt::Dpt()
{}

Dpt::Dpt(short mainGroup, short subGroup, short index /* = 0 */)
    : mainGroup(mainGroup), subGroup(subGroup), index(index)
{
    if (subGroup == 0 && (mainGroup < 14 || mainGroup > 16))
        println("WARNING: You used an invalid Dpt *.0");
}

bool Dpt::operator==(const Dpt& other) const
{
    return other.mainGroup == mainGroup && other.subGroup == subGroup && other.index == index;
}

bool Dpt::operator!=(const Dpt& other) const
{
    return !(other == *this);
}

unsigned char Dpt::dataLength() const
{
    switch (mainGroup)
    {
        case 7:
        case 8:
        case 9:
        case 22:
        case 207:
        case 217:
        case 234:
        case 237:
        case 244:
        case 246:
            return 2;
        case 10:
        case 11:
        case 30:
        case 206:
        case 225:
        case 232:
        case 240:
        case 250:
        case 254:
            return 3;
        case 12:
        case 13:
        case 14:
        case 15:
        case 27:
        case 241:
        case 251:
            return 4;
        case 252:
            return 5;
        case 219:
        case 222:
        case 229:
        case 235:
        case 242:
        case 245:
        case 249:
            return 6;
        case 19:
        case 29:
        case 230:
        case 255:
        case 275:
            return 8;
        case 16:
            return 14; 
        case 285:
            return 16;
        default:
            return 1; // default to 1 byte as currently implemented in GroupObject

    }
}