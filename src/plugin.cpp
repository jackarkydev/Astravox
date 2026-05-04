#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;

	// Register each module — slug must match plugin.json exactly
	p->addModel(modelVocoder);
}
