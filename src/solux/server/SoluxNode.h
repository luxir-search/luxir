#pragma once
#include "solux/store/Directory.h"

namespace solux {

/// There should normally be a single SoluxNode instance per process.
/// A single SoluxNode can host many indexes.
/// There still *may* be multiple SoluxNodes instances per process, but it's currently more for testing.

class SoluxNode {
public:
  RAMDir dir; // temporary singleton index.. should be map from fully_qualified_name to (Directory, existing_writer, existing_reader, schema)
  // fully_qualified_name == tenant+index? (is there a better name than tenant?  what about the case when you have an index
  //  .. or multiple indexes, and it can be read/searched by many users? "library"? archive, repository, vault? room?)
  // OR, we should have a 2 level map structure... top level tenant followed by indexes for that tenant.
  // If we want to support "tables" or multiple document types in a single index, we can incrementally add that later
  //  (table name would be optional, with one main/primary table)

  // what users have access to what libraries / collections... is this outside solux (a layer above?)
  // or should we integrate that?  Should we have actual public API exposure (with api-keys or whatever)
  // or leave that to users of the project?

  // operations:
  // - copy index (or shard level even?).. should index be called collection again?
  // - copy tenant? (like a backup of everything)

private:

};

}