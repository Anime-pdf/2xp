#include "prng.h"

#include "spdlog/spdlog.h"

#define FMT "[Prng] "

// From https://en.wikipedia.org/w/index.php?title=Permuted_congruential_generator&oldid=901497400#Example_code.
//
// > The generator recommended for most users is PCG-XSH-RR with 64-bit state
// > and 32-bit output.

#define NAME "pcg-xsh-rr"

CPrng::CPrng() :
	m_Seeded(false)
{
}

const char *CPrng::Description() const
{
	if(!m_Seeded)
	{
		return NAME ":unseeded";
	}
	return m_aDescription;
}
#undef RotateRight32
static unsigned int RotateRight32(unsigned int x, int Shift)
{
	return (x >> Shift) | (x << (-Shift & 31));
}

void CPrng::Seed(uint64 aSeed[2])
{
	m_Seeded = true;
	str_format(m_aDescription, sizeof(m_aDescription), "%s:%08x%08x:%08x%08x", NAME,
		(unsigned)(aSeed[0] >> 32), (unsigned)aSeed[0],
		(unsigned)(aSeed[1] >> 32), (unsigned)aSeed[1]);

	m_Increment = (aSeed[1] << 1) | 1;
	m_State = aSeed[0] + m_Increment;
	RandomBits();
}

unsigned int CPrng::RandomBits()
{
	if(!m_Seeded)
	{
		spdlog::error(FMT "Prng needs to be seeded before it can generate random numbers");
		return 0;
	}

	uint64 x = m_State;
	unsigned int Count = x >> 59;

	static const uint64 MULTIPLIER = 6364136223846793005u;
	m_State = x * MULTIPLIER + m_Increment;
	x ^= x >> 18;
	return RotateRight32(x >> 27, Count);
}
