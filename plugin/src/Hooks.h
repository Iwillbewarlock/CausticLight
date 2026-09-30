#pragma once

namespace cl::Hooks
{
	// Installs the per-frame main-thread update. Returns false (and writes nothing) when the
	// call site does not look like this build expects.
	bool Install();
}
