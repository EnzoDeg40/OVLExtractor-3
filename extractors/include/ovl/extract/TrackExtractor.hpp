#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Exports the ride-side data of an OVL pair as JSON (docs §15):
//   <ovl>.tracks.json   every TrackSection (tks) in the pair — the piece
//                       catalogue of a track set (e.g. Track20): connection
//                       slope/bank/curve/groups, flags, special type, the
//                       SceneryItem (sid) footprint + svd mesh names, speeds,
//                       animations, and the entry/exit pose measured on the
//                       piece's rail splines (section-local, game frame)
//   <ride>.ride.json    every TrackedRide (trr): the ride's allowed pieces,
//                       trains, lift/station names, speed presets, the
//                       relative path of the track set holding the pieces
//   splines are embedded (nodes + per-segment length) in whichever file
//   references them; unreferenced ones go to <ovl>.splines.json.
// Meshes are not written here — the pieces' svd live in sibling
// `<piece>_data` OVLs that `--types scenery` packages as .glb.
class TrackExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "track"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
