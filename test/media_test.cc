// Unreliable media (docs/design/media-faults.md): reads that fail or return flipped bits,
// once (transient) or always (sticky), at offsets across every file of an index.
//   - A transient fault anywhere heals: the open retries the file, or a search meets
//     corrupt_segment and heal() reads the segment again; the digest equals the
//     undamaged one.
//   - A sticky fault fails the open, reports the segment damaged through heal(), or lands
//     in bytes nothing reads. The digest is never wrong: it equals the undamaged one or
//     the read throws.
//   - A segment damaged for good is recovered from a retained predecessor that does not
//     name it (recover --to).
#include "strata/faultfs.hh"
#include "strata/index.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 30) std::exit(1); } \
	} while (0)

using namespace strata;

static const std::array<char, 16> uuid = {'s', 't', 'r', 'a', 't', 'a', '-', 'm', 'e', 'd', 'i', 'a'};

static operation add(const std::string& key, const std::string& body) {
	operation o;
	o.type = operation::add;
	o.doc.key = key;
	std::uint32_t pos = 0;
	for (std::size_t i = 0; i < body.size(); i += 3) { o.doc.terms.push_back({body.substr(i, 3), 1, {++pos}}); }
	o.doc.length = pos;
	o.doc.stored = body;
	return o;
}
static operation erase(const std::string& key) {
	operation o;
	o.type = operation::delete_key;
	o.key = key;
	return o;
}
static transaction tx(std::vector<operation> ops) {
	transaction t;
	t.ops = std::move(ops);
	return t;
}

// The digest, healing once if a search meets a damaged region; nullopt when the damage
// is on the medium too.
static std::optional<std::uint64_t> read_digest(strata::index& ix, bool& healed) {
	try {
		return ix.digest();
	} catch (const corrupt_segment&) {
	}
	healed = true;
	if (!ix.heal().empty()) { return std::nullopt; }
	return ix.digest();
}

int main() {
	strata::index::options keyed;
	keyed.key_prefix = "Q";
	faultfs::filesystem fs;
	faultfs::io io(fs);
	{
		auto ix = strata::index::create(io, keyed, uuid);
		for (int s = 0; s < 3; ++s) {
			std::vector<operation> ops;
			for (int i = 0; i < 40; ++i) {
				const int k = s * 40 + i;
				ops.push_back(add("Q" + std::to_string(k), "doc" + std::to_string(k) + "alphabetagammadelta" + std::to_string(k * 7919)));
			}
			ix.apply(tx(ops), {});
			ix.refresh();
		}
		ix.apply(tx({erase("Q3"), erase("Q50"), erase("Q99")}), {});
		operation m;
		m.type = operation::set_meta;
		m.meta_key = "schema";
		m.meta_value = std::string(300, 's');
		ix.apply(tx({m}), {});
		ix.commit();
	}
	std::uint64_t reference;
	{
		auto ix = strata::index::open(io, keyed);
		reference = ix.digest();
	}
	std::vector<std::string> names;
	for (const auto& n : fs.list()) {
		if (index_detail::parse_name(n)) { names.push_back(n); }
	}
	std::size_t transient = 0, open_retries = 0, heals = 0;
	std::size_t sticky = 0, sticky_open_failed = 0, sticky_damaged = 0, sticky_unread = 0;
	for (const auto& name : names) {
		const std::uint64_t size = fs.view(name).size();
		const std::uint64_t stride = std::max<std::uint64_t>(1, size / 48);
		// A segment's frozen prefix byte by byte (format-13.md F2), then a stride over the rest.
		const std::uint64_t prefix = name.starts_with("seg-") ? std::min<std::uint64_t>(strata::format::prefix_size, size) : 0;
		std::vector<std::uint64_t> offsets;
		for (std::uint64_t at = 0; at < prefix; ++at) { offsets.push_back(at); }
		for (std::uint64_t at = 0; at < size; at += stride) {
			if (at >= prefix) { offsets.push_back(at); }
		}
		for (const std::uint64_t at : offsets) {
			for (faultfs::media kind : {faultfs::media::bit_flip, faultfs::media::read_error}) {
				// Transient: the medium returns the right bytes on the next read.
				fs.clear_faults();
				fs.add_media_fault(name, kind, at, at + 1, 1);
				try {
					++transient;
					auto ix = strata::index::open(io, keyed);
					bool healed = false;
					const auto d = read_digest(ix, healed);
					CHECK(d && *d == reference);
					if (!d || *d != reference) { std::fprintf(stderr, "  transient %s at %llu of %s\n", kind == faultfs::media::bit_flip ? "flip" : "EIO", (unsigned long long)at, name.c_str()); }
					open_retries += ix.media_retries() && !healed;
					heals += healed;
				} catch (const std::exception& e) {
					CHECK(false);
					std::fprintf(stderr, "  transient %s at %llu of %s: %s\n", kind == faultfs::media::bit_flip ? "flip" : "EIO", (unsigned long long)at, name.c_str(), e.what());
				}
				// Sticky: the medium is damaged for good.
				fs.clear_faults();
				fs.add_media_fault(name, kind, at, at + 1, 0);
				++sticky;
				try {
					auto ix = strata::index::open(io, keyed);
					bool healed = false;
					const auto d = read_digest(ix, healed);
					if (!d) {
						++sticky_damaged;
					} else {
						CHECK(*d == reference);  // never a wrong answer
						++sticky_unread;
					}
					// A merge reads every key and every posting: it fails only as corruption
					// (condition 16), never with another library's error.
					try {
						ix.force_merge(1);
					} catch (const corrupt_segment&) {
					} catch (const corrupt_index&) {
					} catch (const std::exception& e) {
						CHECK(false);
						std::fprintf(stderr, "  merge over sticky %s at %llu of %s: %s\n", kind == faultfs::media::bit_flip ? "flip" : "EIO", (unsigned long long)at, name.c_str(), e.what());
					}
				} catch (const corrupt_index&) {
					++sticky_open_failed;
				} catch (const corrupt_segment&) {
					++sticky_open_failed;
				} catch (const std::exception& e) {
					CHECK(false);
					std::fprintf(stderr, "  sticky %s at %llu of %s: %s\n", kind == faultfs::media::bit_flip ? "flip" : "EIO", (unsigned long long)at, name.c_str(), e.what());
				}
			}
		}
	}
	fs.clear_faults();
	CHECK(transient > 200 && open_retries > 0 && heals > 0 && sticky_open_failed > 0 && sticky_damaged > 0);
	std::printf("  transient: %zu faults, all healed (%zu by the open's retry, %zu by heal())\n", transient, open_retries, heals);
	std::printf("  sticky: %zu faults: %zu failed the open, %zu reported damaged by heal(), %zu in bytes nothing reads\n", sticky, sticky_open_failed,
		sticky_damaged, sticky_unread);
	// A segment damaged for good: the head fails, a retained predecessor that does not
	// name it opens with the same content.
	{
		std::uint64_t before;
		{
			auto ix = strata::index::open(io, keyed);
			before = ix.manifest().generation;
			ix.force_merge(1);
			ix.commit();
			CHECK(ix.digest() == reference);
		}
		std::string merged;
		{
			auto ix = strata::index::open(io, keyed);
			merged = index_detail::segment_name(ix.manifest().segments.at(0).id);
		}
		const std::uint64_t size = fs.view(merged).size();
		fs.add_media_fault(merged, faultfs::media::bit_flip, size / 2, size / 2 + 1, 0);
		bool head_failed = false;
		try {
			auto ix = strata::index::open(io, keyed);
			bool healed = false;
			head_failed = !read_digest(ix, healed);
		} catch (const corrupt_index&) {
			head_failed = true;
		} catch (const corrupt_segment&) {
			head_failed = true;
		}
		CHECK(head_failed);
		auto ix = strata::index::open(io, keyed, before);
		CHECK(ix.digest() == reference);
	}
	std::printf("media: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
