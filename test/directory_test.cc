// The index directory (small-indexes D2 and D3-C): its owner lock is the directory's own, one
// writer per directory within a process and across processes (T5); a pre-D2 index is refused
// before anything is written, even while a pre-D2 writer holds its owner.lock (T5b); lock
// errors other than busy refuse with nothing written, and a filesystem not known local or a
// lock it does not support warns or refuses as the index's policy says (T7); a standalone
// create under power loss never leaves an index that neither open() nor create() accepts
// (T11's standalone half).
//
//   directory_test STATE_DIR [tmpfs]
//   directory_test opener DIR open|create PARENT_DIR_FD   (the spawned second process)
#include "completion/posix.h"
#include "text_field.hh"
#include "prospect/index.hh"
#include "strata/core/faultfs.hh"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

extern char** environ;

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 40) std::exit(1); } \
	} while (0)

using namespace prospect;
namespace fs = std::filesystem;

static const std::array<char, 16> uuid_a = {'d', 'i', 'r', 'e', 'c', 't', 'o', 'r', 'y', 0, 0, 0, 0, 0, 0, 1};

static index::options keyed() {
	index::options o;
	o.keyed = true;
	return o;
}

static transaction one_doc(const std::string& key) {
	operation o;
	o.type = operation::replace_key;
	o.key = key;
	o.doc.key = key;
	o.doc.field().terms.push_back({"alpha", 1, {1}});
	o.doc.length = 1;
	o.doc.stored = "s";
	transaction t;
	t.fields = text_fields();
	t.ops.push_back(o);
	return t;
}

// An empty directory PosixIO accepts: owned, not group or world writable.
static void fresh(const fs::path& dir) {
	fs::remove_all(dir);
	fs::create_directories(dir);
	fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
}

// Every entry's name and bytes: "nothing written" is this, unchanged.
static std::map<std::string, std::string> listing(const fs::path& dir) {
	std::map<std::string, std::string> out;
	for (const auto& e : fs::directory_iterator(dir)) {
		std::ifstream in(e.path(), std::ios::binary);
		out[e.path().filename().string()] = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
	return out;
}

// The system_error's code a call throws: 0 when it returns, -1 for any other exception.
static int error_of(const std::function<void()>& f, std::string* what = nullptr) {
	try {
		f();
	} catch (const std::system_error& e) {
		if (what) { *what = e.what(); }
		return e.code().value();
	} catch (const std::exception& e) {
		if (what) { *what = e.what(); }
		return -1;
	}
	return 0;
}

static void open_or_create(kronuz::journal::IO& io, const std::string& how) {
	if (how == "create") {
		auto ix = index::create(io, keyed(), uuid_a);
	} else {
		auto ix = index::open(io, keyed());
	}
}

// --- the second process --------------------------------------------------------------------

enum opener_exit { busy = 10, opened = 11, other = 12, inherited = 13 };

static int opener(const char* dir, const std::string& how, int parent_fd) {
	// The parent's directory descriptor must not be here: an inherited open description
	// would share the parent's lock instead of contending for it.
	struct stat mine{}, theirs{};
	if (::stat(dir, &mine) == 0 && ::fstat(parent_fd, &theirs) == 0 && mine.st_dev == theirs.st_dev && mine.st_ino == theirs.st_ino) { return inherited; }
	kronuz::journal::PosixIO io(dir);
	const int e = error_of([&] { open_or_create(io, how); });
	return e == 0 ? opened : e == EWOULDBLOCK ? busy : other;
}

static int spawn_opener(const char* self, const fs::path& dir, const std::string& how, int parent_fd) {
	const std::string d = dir.string(), fd = std::to_string(parent_fd);
	const char* args[] = {self, "opener", d.c_str(), how.c_str(), fd.c_str(), nullptr};
	pid_t pid = 0;
	if (::posix_spawn(&pid, self, nullptr, nullptr, const_cast<char* const*>(args), environ) != 0) { return -1; }
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// An IO that forwards to another, as a host's metrics or quota layer would. With classify
// off it leaves directory_filesystem to the base, which reports the filesystem unclassified.
class wrapper_io final : public kronuz::journal::IO {
public:
	wrapper_io(kronuz::journal::IO& inner, bool classify) : inner_(&inner), classify_(classify) {}
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_owner(bool create) override { return inner_->acquire_owner(create); }
	std::unique_ptr<kronuz::journal::OwnerLock> acquire_directory_owner() override { return inner_->acquire_directory_owner(); }
	kronuz::journal::FilesystemClass directory_filesystem() const override {
		return classify_ ? inner_->directory_filesystem() : kronuz::journal::IO::directory_filesystem();
	}
	kronuz::journal::DirectoryTrust directory_trust() const override {
		return classify_ ? inner_->directory_trust() : kronuz::journal::IO::directory_trust();
	}
	std::unique_ptr<kronuz::journal::File> open_existing(std::string_view name) override { return inner_->open_existing(name); }
	std::unique_ptr<kronuz::journal::File> create_exclusive(std::string_view name) override { return inner_->create_exclusive(name); }
	void replace(std::string_view source, std::string_view destination) override { inner_->replace(source, destination); }
	void remove(std::string_view name) override { inner_->remove(name); }
	std::unique_ptr<kronuz::journal::DirectoryCursor> scan_directory() override { return inner_->scan_directory(); }
	void sync_directory() override { inner_->sync_directory(); }
	void sync_files(std::span<kronuz::journal::File* const> files) override { inner_->sync_files(files); }

private:
	kronuz::journal::IO* inner_;
	bool classify_;
};

// --- T7's injections -----------------------------------------------------------------------

// The lock's answer: 0 for the real flock, else that errno.
static int flock_errno = 0;
static int fake_flock(int fd, int operation) {
	if (!flock_errno) { return ::flock(fd, operation); }
	errno = flock_errno;
	return -1;
}

#ifdef KRONUZ_COMPLETION_STATFS
struct fake_fs {
	const char* expect;  // the type name the warning or refusal carries
	bool fuse;
#if defined(__linux__)
	std::uint32_t magic;
#else
	const char* name;
	bool local;  // MNT_LOCAL, as the filesystem claims it
#endif
};
static const fake_fs* faking = nullptr;
static int fake_errno = 0;
static int fake_statfs(int, struct statfs* out) {
	if (fake_errno) {
		errno = fake_errno;
		return -1;
	}
	std::memset(out, 0, sizeof *out);
#if defined(__linux__)
	out->f_type = static_cast<decltype(out->f_type)>(faking->magic);
#else
	std::snprintf(out->f_fstypename, sizeof out->f_fstypename, "%s", faking->name);
	out->f_flags = faking->local ? MNT_LOCAL : 0;
#endif
	return 0;
}
#endif

static index::options policy(index::options::nonlocal_policy p) {
	auto o = keyed();
	o.nonlocal = p;
	return o;
}
static constexpr auto warn = index::options::nonlocal_policy::warn;
static constexpr auto refuse = index::options::nonlocal_policy::refuse;

static index::options trust_policy(index::options::trust_policy p) {
	auto o = keyed();
	o.directory_trust = p;
	return o;
}
static constexpr auto trust_warn = index::options::trust_policy::warn;
static constexpr auto trust_refuse = index::options::trust_policy::refuse;
// What a host that wants to warn constructs its PosixIO with.
static constexpr kronuz::journal::PosixIOOptions admit_all{true, true};

int main(int argc, char** argv) {
	if (argc == 5 && std::string(argv[1]) == "opener") { return opener(argv[2], argv[3], std::atoi(argv[4])); }
	if (argc < 2) {
		std::fprintf(stderr, "usage: directory_test STATE_DIR [tmpfs]\n");
		return 2;
	}
	const fs::path root = argv[1];
	const bool disk_only = argc > 2;  // a second filesystem, for the on-disk cases alone
	fresh(root);
#if defined(__linux__)
	if (argc > 2 && std::string(argv[2]) == "tmpfs") {
		struct statfs s{};
		CHECK(::statfs(root.c_str(), &s) == 0 && static_cast<std::uint32_t>(s.f_type) == 0x01021994u);
	}
#endif

	// T5: one writer per directory. Each pair as two descriptions in this process and as a
	// spawned second process; the second fails busy and writes nothing, and once the first
	// closes, an open succeeds.
	for (const std::string first : {"open", "create"}) {
		for (const std::string second : {"open", "create"}) {
			const fs::path dir = root / ("t5-" + first + "-" + second);
			fresh(dir);
			if (first == "open") {
				kronuz::journal::PosixIO setup(dir);
				auto ix = index::create(setup, keyed(), uuid_a);
				ix.apply(one_doc("Qa"), {});
				ix.commit();
			}
			{
				kronuz::journal::PosixIO io1(dir);
				std::optional<prospect::index> held;
				if (first == "create") {
					held.emplace(index::create(io1, keyed(), uuid_a));
				} else {
					held.emplace(index::open(io1, keyed()));
				}
				const auto before = listing(dir);
				CHECK(!before.count("owner.lock"));  // the lock writes no file
				kronuz::journal::PosixIO io2(dir);
				CHECK(error_of([&] { open_or_create(io2, second); }) == EWOULDBLOCK);
				CHECK(listing(dir) == before);
				CHECK(spawn_opener(argv[0], dir, second, io1.native_directory_handle()) == busy);
				CHECK(listing(dir) == before);
			}
			kronuz::journal::PosixIO io3(dir);
			CHECK(error_of([&] { open_or_create(io3, "open"); }) == 0);
			CHECK(spawn_opener(argv[0], dir, "open", io3.native_directory_handle()) == opened);
		}
	}

	// T5b: a pre-D2 index (files family 0x80000004, owner.lock) is refused as
	// unsupported_index before anything is written, while another description holds its
	// owner.lock throughout as a pre-D2 writer would; without the files bump this open would
	// succeed under the directory lock alone, a second writer (SI-R27).
	{
		const fs::path dir = root / "t5b";
		fresh(dir);
		for (const auto& e : fs::directory_iterator(fs::path(PROSPECT_FIXTURES) / "pre-d2")) {
			fs::copy_file(e.path(), dir / e.path().filename());
			fs::permissions(dir / e.path().filename(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
		}
		const auto before = listing(dir);
		CHECK(before.count("owner.lock") && before.count("manifest-2"));
		// The fixture's manifest carries the pre-D2 version, LEB128 after the magic.
		CHECK(before.at("manifest-2").substr(8, 5) == std::string("\x84\x80\x80\x80\x08", 5));
		CHECK(files::pre_freeze != 0x8000'0004);  // D2 bumps the files family (F20)
		kronuz::journal::PosixIO old_binary(dir);
		auto old_lock = old_binary.acquire_owner(false);
		kronuz::journal::PosixIO io(dir);
		bool unsupported = false;
		try {
			auto ix = index::open(io, keyed());
		} catch (const unsupported_index&) {
			unsupported = true;
		} catch (const std::exception& e) {
			std::fprintf(stderr, "t5b open: %s\n", e.what());
		}
		CHECK(unsupported);
		CHECK(listing(dir) == before);
		CHECK(error_of([&] { auto ix = index::create(io, keyed(), uuid_a); }) == EEXIST);
		CHECK(listing(dir) == before);
	}

	// T7: lock errors other than busy and unsupported refuse as they are under either policy,
	// with nothing written (SI-R27).
	for (const auto p : {warn, refuse}) {
		for (const int error : {ENOLCK, EINVAL, EBADF}) {
			const fs::path dir = root / "t7-lock";
			fresh(dir);
			{
				kronuz::journal::PosixIO setup(dir);
				auto ix = index::create(setup, keyed(), uuid_a);
				CHECK(!ix.filesystem_warning());  // a known-local filesystem
			}
			const auto made = listing(dir);
			flock_errno = error;
			kronuz::journal::PosixIO io(dir);
			io.replace_flock(fake_flock);
			CHECK(error_of([&] { auto ix = index::open(io, policy(p)); }) == error);
			CHECK(listing(dir) == made);
			fresh(dir);
			kronuz::journal::PosixIO empty(dir);
			empty.replace_flock(fake_flock);
			CHECK(error_of([&] { auto ix = index::create(empty, policy(p), uuid_a); }) == error);
			CHECK(listing(dir).empty());
			flock_errno = 0;
		}
	}
	// T7: a lock the filesystem does not support: warn proceeds unlocked and says so; refuse
	// refuses with not_supported, nothing written (the owner's ruling, F20).
	for (const int error : {ENOTSUP, EOPNOTSUPP}) {
		const fs::path dir = root / "t7-unsupported";
		fresh(dir);
		flock_errno = error;
		{
			kronuz::journal::PosixIO io(dir);
			io.replace_flock(fake_flock);
			CHECK(error_of([&] { auto ix = index::create(io, policy(refuse), uuid_a); }) == int(std::errc::not_supported));
			CHECK(listing(dir).empty());
			auto ix = index::create(io, policy(warn), uuid_a);
			const auto& w = ix.filesystem_warning();
			CHECK(w && w->local && !w->locked);
		}
		const auto made = listing(dir);
		kronuz::journal::PosixIO io(dir);
		io.replace_flock(fake_flock);
		CHECK(error_of([&] { auto ix = index::open(io, policy(refuse)); }) == int(std::errc::not_supported));
		CHECK(listing(dir) == made);
		{
			auto ix = index::open(io, policy(warn));
			CHECK(ix.filesystem_warning() && !ix.filesystem_warning()->locked);
		}
		flock_errno = 0;
	}
#ifdef KRONUZ_COMPLETION_STATFS
	// T7: a filesystem not known local. warn opens and creates, locked, and reports it; refuse
	// refuses with not_supported naming the type and writes nothing, except FUSE's type, never
	// refused (the lead's call), still reported. With the lock unsupported as well, warn
	// proceeds unlocked and refuse refuses, FUSE included. A failed statfs refuses with its own
	// error.
	{
#if defined(__linux__)
		static const fake_fs cases[] = {{"nfs", false, 0x6969}, {"smb2", false, 0xFE534D42}, {"cifs", false, 0xFF534D42}, {"fuse", true, 0x65735546},
			{"9p", false, 0x01021997}, {"unknown type 0x12345678", false, 0x12345678}};
#else
		static const fake_fs cases[] = {{"nfs", false, "nfs", false}, {"smbfs", false, "smbfs", false}, {"macfuse", true, "macfuse", false},
			{"fusefs", true, "fusefs", false}, {"9p", false, "9p", false}, {"mystery", false, "mystery", false}};
#endif
		const fs::path dir = root / "t7-statfs";
		for (const auto& c : cases) {
			faking = &c;
			for (const bool unsupported : {false, true}) {
				flock_errno = unsupported ? ENOTSUP : 0;
				auto io_for = [&](kronuz::journal::PosixIO& io) {
					io.replace_statfs(fake_statfs);
					io.replace_flock(fake_flock);
				};
				auto warned = [&](const prospect::index& ix) {
					const auto& w = ix.filesystem_warning();
					return w && w->filesystem == c.expect && !w->local && w->locked == !unsupported;
				};
				fresh(dir);
				{
					kronuz::journal::PosixIO io(dir);
					io_for(io);
					std::string what;
					const int e = error_of([&] { CHECK(warned(index::create(io, policy(refuse), uuid_a))); }, &what);
					if (c.fuse && !unsupported) {
						CHECK(e == 0);  // the index is created; warn's create is the others' case
					} else {
						CHECK(e == int(std::errc::not_supported) && what.find(c.expect) != std::string::npos);
						CHECK(listing(dir).empty());
						CHECK(error_of([&] { CHECK(warned(index::create(io, policy(warn), uuid_a))); }) == 0);
					}
				}
				const auto made = listing(dir);
				kronuz::journal::PosixIO io(dir);
				io_for(io);
				std::string what;
				const int e = error_of([&] { CHECK(warned(index::open(io, policy(refuse)))); }, &what);
				CHECK(c.fuse && !unsupported ? e == 0 : e == int(std::errc::not_supported) && what.find(c.expect) != std::string::npos);
				CHECK(listing(dir) == made);
				CHECK(error_of([&] { CHECK(warned(index::open(io, policy(warn)))); }) == 0);
				CHECK(listing(dir) == made);
				flock_errno = 0;
			}
		}
#if !defined(__linux__)
		// A FUSE mount claiming MNT_LOCAL is local: the kernel flag is the classifier's evidence.
		static const fake_fs claimed{"macfuse", true, "macfuse", true};
		faking = &claimed;
		fresh(dir);
		{
			kronuz::journal::PosixIO io(dir);
			io.replace_statfs(fake_statfs);
			CHECK(!index::create(io, policy(refuse), uuid_a).filesystem_warning());
		}
#endif
		// A failed statfs: warn proceeds, locked, and names the failure as an unknown filesystem;
		// refuse refuses with the failure's own error (the lead's ruling, F20).
		const auto made = listing(dir);
		fake_errno = EIO;
		{
			kronuz::journal::PosixIO io(dir);
			io.replace_statfs(fake_statfs);
			std::string what;
			CHECK(error_of([&] { auto ix = index::open(io, policy(refuse)); }, &what) == EIO);
			CHECK(what.find("unknown") != std::string::npos && what.find("inspect") != std::string::npos);
			CHECK(listing(dir) == made);
			auto warned = [&](const prospect::index& ix) {
				const auto& w = ix.filesystem_warning();
				return w && w->filesystem.find("unknown") != std::string::npos && w->filesystem.find("inspect") != std::string::npos && !w->local && w->locked;
			};
			CHECK(error_of([&] { CHECK(warned(index::open(io, policy(warn)))); }) == 0);
			CHECK(listing(dir) == made);
			CHECK(error_of([&] { auto ix = index::create(io, policy(refuse), uuid_a); }) == EIO);
			CHECK(listing(dir) == made);
		}
		fresh(dir);
		{
			kronuz::journal::PosixIO io(dir);
			io.replace_statfs(fake_statfs);
			auto ix = index::create(io, policy(warn), uuid_a);
			const auto& w = ix.filesystem_warning();
			CHECK(w && w->filesystem.find("unknown") != std::string::npos && !w->local && w->locked);
		}
		fake_errno = 0;
	}
	// T7: refuse is enforced through a wrapper that forwards the classification: a PosixIO on
	// NFS refuses as bare, and warn reports it. A wrapper that does not forward is unclassified,
	// so not local: refuse refuses it and warn warns (DL2).
	{
		static const fake_fs nfs{"nfs", false,
#if defined(__linux__)
			0x6969};
#else
			"nfs", false};
#endif
		faking = &nfs;
		const fs::path dir = root / "t7-wrapper";
		fresh(dir);
		kronuz::journal::PosixIO inner(dir);
		inner.replace_statfs(fake_statfs);
		wrapper_io forwarding(inner, true);
		CHECK(error_of([&] { auto ix = index::create(forwarding, policy(refuse), uuid_a); }) == int(std::errc::not_supported));
		CHECK(listing(dir).empty());
		{
			auto ix = index::create(forwarding, policy(warn), uuid_a);
			CHECK(ix.filesystem_warning() && ix.filesystem_warning()->filesystem == "nfs" && ix.filesystem_warning()->locked);
		}
		const auto made = listing(dir);
		CHECK(error_of([&] { auto ix = index::open(forwarding, policy(refuse)); }) == int(std::errc::not_supported));
		CHECK(listing(dir) == made);
		wrapper_io bare(inner, false);
		std::string what;
		CHECK(error_of([&] { auto ix = index::open(bare, policy(refuse)); }, &what) == int(std::errc::not_supported));
		CHECK(what.find("unclassified") != std::string::npos);
		CHECK(listing(dir) == made);
		auto ix = index::open(bare, policy(warn));
		CHECK(ix.filesystem_warning() && ix.filesystem_warning()->filesystem == "unclassified" && !ix.filesystem_warning()->local &&
			ix.filesystem_warning()->locked);
		faking = nullptr;
	}
#endif

	// T7 on faultfs, which models a local disk: classified local by default, so no warning;
	// classified NFS, it refuses under refuse and warns under warn, through a wrapper too.
	{
		faultfs::filesystem f;
		faultfs::io io(f);
		{
			auto ix = index::create(io, policy(refuse), uuid_a);
			CHECK(!ix.filesystem_warning());
		}
		f.classify({false, "nfs"});
		wrapper_io forwarding(io, true);
		CHECK(error_of([&] { auto ix = index::open(io, policy(refuse)); }) == int(std::errc::not_supported));
		CHECK(error_of([&] { auto ix = index::open(forwarding, policy(refuse)); }) == int(std::errc::not_supported));
		auto ix = index::open(forwarding, policy(warn));
		CHECK(ix.filesystem_warning() && ix.filesystem_warning()->filesystem == "nfs");
	}

	// Directory trust (F20): a symlinked root proceeds silently; a directory another user owns
	// or group or others can write proceeds with directory_trust_warning() under warn and
	// refuses with permission_denied, nothing written, under refuse. The PosixIO defaults still
	// refuse every case before strata sees it.
	{
		const fs::path base = fs::absolute(root) / "trust";
		fresh(base);
		const fs::path target = base / "target", link = base / "link";
		struct shape { const char* name; fs::perms mode; };
		const shape shapes[] = {{"private", fs::perms::owner_all},
			{"group-writable", fs::perms::owner_all | fs::perms::group_write},
			{"other-writable", fs::perms::owner_all | fs::perms::others_write},
			{"group- and other-writable", fs::perms::owner_all | fs::perms::group_write | fs::perms::others_write}};
		for (const bool symlinked : {false, true}) {
			for (const auto& shape : shapes) {
				const bool shared = shape.mode != fs::perms::owner_all;
				fresh(target);
				fs::permissions(target, shape.mode, fs::perm_options::replace);
				fs::remove(link);
				if (symlinked) { fs::create_directory_symlink(target, link); }
				const fs::path dir = symlinked ? link : target;
				// The library's defaults refuse a symlinked root and a shared one.
				CHECK((error_of([&] { kronuz::journal::PosixIO io(dir); }) != 0) == (symlinked || shared));
				if (symlinked) { CHECK(error_of([&] { kronuz::journal::PosixIO io(dir, 0600, {false, true}); }) != 0); }
				if (shared) { CHECK(error_of([&] { kronuz::journal::PosixIO io(dir, 0600, {true, false}); }) != 0); }
				auto warned = [&](const prospect::index& ix) {
					const auto& w = ix.directory_trust_warning();
					return w && w->known && !w->foreign_owner && w->shared_writable;
				};
				// warn: create, then open, proceed; only a shared mode is reported.
				{
					kronuz::journal::PosixIO io(dir, 0600, admit_all);
					CHECK(io.directory_trust().symlinked_root == symlinked && io.directory_trust().shared_writable == shared);
					auto ix = index::create(io, trust_policy(trust_warn), uuid_a);
					CHECK(shared ? warned(ix) : !ix.directory_trust_warning());
					CHECK(!ix.filesystem_warning());
					ix.apply(one_doc("Qa"), {});
					ix.commit();
				}
				CHECK(!listing(target).empty());  // the index landed in the symlink's target
				{
					kronuz::journal::PosixIO io(dir, 0600, admit_all);
					auto ix = index::open(io, keyed());  // warn is the default
					CHECK(shared ? warned(ix) : !ix.directory_trust_warning());
					CHECK(!ix.filesystem_warning());
					CHECK(index::options{}.directory_trust == trust_warn);
				}
				// refuse: a shared directory refuses before anything is written; the rest opens.
				const auto made = listing(target);
				{
					kronuz::journal::PosixIO io(dir, 0600, admit_all);
					std::string what;
					const int e = error_of([&] { auto ix = index::open(io, trust_policy(trust_refuse)); }, &what);
					if (shared) {
						CHECK(e == int(std::errc::permission_denied) && what.find("group or others") != std::string::npos);
					} else {
						CHECK(e == 0);
					}
					CHECK(listing(target) == made);
					fresh(target);
					fs::permissions(target, shape.mode, fs::perm_options::replace);
					kronuz::journal::PosixIO empty(dir, 0600, admit_all);
					CHECK(error_of([&] { auto ix = index::create(empty, trust_policy(trust_refuse), uuid_a); }) == (shared ? int(std::errc::permission_denied) : 0));
					CHECK(shared ? listing(target).empty() : !listing(target).empty());
				}
			}
		}
		fs::remove(link);

		// A foreign owner, which an ordinary user cannot make on disk, and an IO that does not
		// say, through faultfs and the wrappers: warn reports it, refuse refuses.
		{
			faultfs::filesystem f;
			faultfs::io io(f);
			{
				auto ix = index::create(io, trust_policy(trust_refuse), uuid_a);  // owned and private: trusted
				CHECK(!ix.directory_trust_warning());
			}
			const kronuz::journal::DirectoryTrust owned_elsewhere{true, false, true, false};
			f.set_trust(owned_elsewhere);
			{
				auto ix = index::open(io, trust_policy(trust_warn));
				CHECK(ix.directory_trust_warning() && ix.directory_trust_warning()->known && ix.directory_trust_warning()->foreign_owner);
			}
			wrapper_io forwarding(io, true);
			{
				auto ix = index::open(forwarding, trust_policy(trust_warn));
				CHECK(ix.directory_trust_warning() && ix.directory_trust_warning()->foreign_owner && !ix.directory_trust_warning()->shared_writable);
			}
			const auto before = f.mutations();
			std::string what;
			CHECK(error_of([&] { auto ix = index::open(io, trust_policy(trust_refuse)); }, &what) == int(std::errc::permission_denied));
			CHECK(what.find("another user") != std::string::npos);
			CHECK(error_of([&] { auto ix = index::open(forwarding, trust_policy(trust_refuse)); }) == int(std::errc::permission_denied));
			CHECK(f.mutations() == before);
			// Both facts at once, and with the filesystem warning independent of the trust one.
			f.set_trust({true, false, true, true});
			f.classify({false, "nfs"});
			{
				auto ix = index::open(forwarding, trust_policy(trust_warn));
				CHECK(ix.directory_trust_warning() && ix.directory_trust_warning()->foreign_owner && ix.directory_trust_warning()->shared_writable);
				CHECK(ix.filesystem_warning() && ix.filesystem_warning()->filesystem == "nfs");
			}
			// A symlinked root alone sets nothing, and an index created there proceeds under refuse.
			f.set_trust({true, true, false, false});
			f.classify({true, "faultfs"});
			auto quiet = index::open(forwarding, trust_policy(trust_refuse));
			CHECK(!quiet.directory_trust_warning() && !quiet.filesystem_warning());
		}
		// A wrapper that does not forward leaves the base default: unknown, so not trusted.
		{
			faultfs::filesystem f;
			faultfs::io io(f);
			{ auto ix = index::create(io, keyed(), uuid_a); }
			wrapper_io bare(io, false);
			std::string what;
			CHECK(error_of([&] { auto ix = index::open(bare, trust_policy(trust_refuse)); }, &what) == int(std::errc::permission_denied));
			CHECK(what.find("unknown") != std::string::npos);
			auto ix = index::open(bare, trust_policy(trust_warn));
			CHECK(ix.directory_trust_warning() && !ix.directory_trust_warning()->known);
		}
		fs::remove_all(base);
	}

	if (!disk_only) {  // faultfs: the first filesystem's run covers it
		// DL1: nothing is written before the lock. A create over residue and an open beside a
		// writer's temporary file, both against a held lock, are busy with the files untouched
		// (recovery would remove the holder's in-flight files).
		{
			faultfs::filesystem f;
			faultfs::io holder(f), other(f);
			auto lock = holder.acquire_directory_owner();  // a create in progress, nothing written yet
			other.create_exclusive("meta-1.kv")->write_at(0, "torn");
			other.sync_directory();
			const auto before = f.mutations();
			CHECK(error_of([&] { auto ix = index::create(other, keyed(), uuid_a); }) == EWOULDBLOCK);
			CHECK(f.mutations() == before);
			CHECK(f.list().size() == 1);
		}
		{
			faultfs::filesystem f;
			faultfs::io holder(f), other(f);
			auto ix = index::create(holder, keyed(), uuid_a);
			ix.apply(one_doc("Qa"), {});
			ix.commit();
			other.create_exclusive("manifest-9.tmp")->write_at(0, "in flight");  // the holder's commit
			const auto before = f.mutations();
			const auto names = f.list();
			CHECK(error_of([&] { auto o = index::open(other, keyed()); }) == EWOULDBLOCK);
			CHECK(f.mutations() == before);
			CHECK(f.list() == names);
		}

		// D3-C: a standalone create is the deferred create (the directory lock and clearing an
		// interrupted create's residue, writing nothing) and then the first commit, so a create
		// over an empty directory makes 4 durability calls: 2 file syncs (the metadata file, the
		// manifest) and 2 directory syncs. Clearing residue adds one directory sync.
		{
			auto syncs = [](faultfs::op kind, std::uint64_t nth, bool residue) {
				faultfs::filesystem f;
				faultfs::io io(f);
				if (residue) {
					io.create_exclusive("meta-1.kv")->write_at(0, "torn");
					io.sync_directory();
				}
				f.add_fault({kind, nth, EIO});
				return error_of([&] { auto ix = index::create(io, keyed(), uuid_a); }) != 0;  // the nth happened
			};
			CHECK(syncs(faultfs::op::sync, 2, false) && !syncs(faultfs::op::sync, 3, false));
			CHECK(syncs(faultfs::op::sync_directory, 2, false) && !syncs(faultfs::op::sync_directory, 3, false));
			CHECK(syncs(faultfs::op::sync_directory, 3, true) && !syncs(faultfs::op::sync_directory, 4, true));
			// Power off before any mutation: create fails and the directory stays empty.
			faultfs::filesystem f;
			faultfs::io io(f);
			f.power_off_after(0);
			CHECK(error_of([&] { auto ix = index::create(io, keyed(), uuid_a); }) != 0);
			CHECK(f.list().empty());
		}

		// T11, the standalone half: 20,000 seeded power losses over a create and its first commit,
		// at every mutation; each outcome opens (the empty index or the committed one) or reads as
		// never created and create() clears it. Never corrupt_index (SI-R22's 24.8% without syncs).
		{
			std::uint64_t total = 0, empty = 0, one = 0;
			{
				faultfs::filesystem f;
				faultfs::io io(f);
				auto ix = index::create(io, keyed(), uuid_a);
				empty = ix.digest();
				ix.apply(one_doc("Qa"), {});
				ix.commit();
				one = ix.digest();
				total = f.mutations();
			}
			const std::uint64_t runs = 20000, per = (runs + total) / (total + 1);
			std::uint64_t n = 0, opened_empty = 0, opened_one = 0, cleared = 0, corrupt = 0, other_errors = 0;
			for (std::uint64_t at = 0; at <= total; ++at) {
				for (std::uint64_t k = 0; k < per; ++k, ++n) {
					faultfs::filesystem f;
					faultfs::io io(f);
					f.power_off_after(at);
					try {
						auto ix = index::create(io, keyed(), uuid_a);
						ix.apply(one_doc("Qa"), {});
						ix.commit();
					} catch (const std::exception&) {
					}
					f.crash(n * 7919 + 13);
					try {
						const auto d = index::open(io, keyed()).digest();
						opened_empty += d == empty;
						opened_one += d == one;
						CHECK(d == empty || d == one);
					} catch (const index_not_created&) {
						++cleared;
						CHECK(error_of([&] { CHECK(index::create(io, keyed(), uuid_a).digest() == empty); }) == 0);
					} catch (const corrupt_index& e) {
						if (corrupt++ < 5) { std::fprintf(stderr, "t11: power off after %llu, seed %llu: %s\n", (unsigned long long)at, (unsigned long long)n, e.what()); }
					} catch (const std::exception& e) {
						if (other_errors++ < 5) { std::fprintf(stderr, "t11: power off after %llu: %s\n", (unsigned long long)at, e.what()); }
					}
				}
			}
			std::printf("  T11: %llu power losses over %llu mutations: %llu opened empty, %llu opened committed, %llu cleared by create, %llu corrupt\n",
				(unsigned long long)n, (unsigned long long)total, (unsigned long long)opened_empty, (unsigned long long)opened_one, (unsigned long long)cleared,
				(unsigned long long)corrupt);
			CHECK(n >= runs && corrupt == 0 && other_errors == 0);
			CHECK(opened_empty && opened_one && cleared);
		}
	}

	fs::remove_all(root);
	std::printf("directory: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
