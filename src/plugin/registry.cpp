//---------------------------------------------------------------------------
// Registry 実装
//---------------------------------------------------------------------------
#include "plugin/registry.h"
#include "appserve/log.h"
#include "core/util.h"

namespace appserve {

//---------------------------------------------------------------------------
void Registry::setOwner(std::string owner)
{
	std::lock_guard<std::mutex> lk(mu_);
	owner_ = std::move(owner);
}

//---------------------------------------------------------------------------
void Registry::route(std::string prefix, Affinity affinity, Handler handler)
{
	if (prefix.empty() || prefix[0] != '/') {
		logW("Registry: route prefix must start with '/': " + prefix);
		return;
	}
	if (!handler) {
		logW("Registry: null handler for " + prefix);
		return;
	}
	auto r = std::make_shared<Route>();
	r->prefix   = std::move(prefix);
	r->affinity = affinity;
	r->handler  = std::move(handler);

	std::lock_guard<std::mutex> lk(mu_);
	r->owner = owner_;
	for (auto& e : routes_) {
		if (e->prefix == r->prefix) { e = r; return; }   // 同一 prefix は上書き
	}
	routes_.push_back(std::move(r));
}

bool Registry::unroute(const std::string& prefix)
{
	std::lock_guard<std::mutex> lk(mu_);
	for (size_t i = 0; i < routes_.size(); ++i) {
		if (routes_[i]->prefix == prefix) {
			routes_.erase(routes_.begin() + (long)i);
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------
void Registry::mountDir(std::string prefix, std::string dir)
{
	if (prefix.empty() || prefix[0] != '/') {
		logW("Registry: mount prefix must start with '/': " + prefix);
		return;
	}
	if (prefix.back() != '/') prefix += '/';
	auto m = std::make_shared<DirMount>();
	m->prefix = std::move(prefix);
	m->dir    = util::normalizeSlash(std::move(dir));

	std::lock_guard<std::mutex> lk(mu_);
	m->owner = owner_;
	for (auto& e : mounts_) {
		if (e->prefix == m->prefix) { e = m; return; }
	}
	mounts_.push_back(std::move(m));
}

bool Registry::unmountDir(const std::string& prefix)
{
	std::lock_guard<std::mutex> lk(mu_);
	for (size_t i = 0; i < mounts_.size(); ++i) {
		if (mounts_[i]->prefix == prefix) {
			mounts_.erase(mounts_.begin() + (long)i);
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------
void Registry::replCommand(std::string name, std::string help, ReplFn fn,
                           Affinity affinity)
{
	if (name.empty() || !fn) return;
	if (name[0] == '.') name.erase(0, 1);
	auto c = std::make_shared<ReplCmd>();
	c->name     = std::move(name);
	c->help     = std::move(help);
	c->fn       = std::move(fn);
	c->affinity = affinity;

	std::lock_guard<std::mutex> lk(mu_);
	c->owner = owner_;
	for (auto& e : repl_) {
		if (e->name == c->name) { e = c; return; }
	}
	repl_.push_back(std::move(c));
}

//---------------------------------------------------------------------------
std::shared_ptr<const Route> Registry::matchRoute(const std::string& path) const
{
	std::lock_guard<std::mutex> lk(mu_);
	std::shared_ptr<const Route> best;
	for (const auto& r : routes_) {
		if (path.size() < r->prefix.size()) continue;
		if (path.compare(0, r->prefix.size(), r->prefix) != 0) continue;
		if (!best || r->prefix.size() > best->prefix.size()) best = r;
	}
	return best;
}

std::shared_ptr<const DirMount> Registry::matchMount(const std::string& path) const
{
	std::lock_guard<std::mutex> lk(mu_);
	std::shared_ptr<const DirMount> best;
	for (const auto& m : mounts_) {
		if (path.size() < m->prefix.size()) continue;
		if (path.compare(0, m->prefix.size(), m->prefix) != 0) continue;
		if (!best || m->prefix.size() > best->prefix.size()) best = m;
	}
	return best;
}

//---------------------------------------------------------------------------
std::vector<std::shared_ptr<const Route>> Registry::routes() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return std::vector<std::shared_ptr<const Route>>(routes_.begin(), routes_.end());
}

std::vector<std::shared_ptr<const DirMount>> Registry::mounts() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return std::vector<std::shared_ptr<const DirMount>>(mounts_.begin(), mounts_.end());
}

std::vector<std::shared_ptr<const ReplCmd>> Registry::replCommands() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return std::vector<std::shared_ptr<const ReplCmd>>(repl_.begin(), repl_.end());
}

std::shared_ptr<const ReplCmd> Registry::findReplCommand(const std::string& name) const
{
	std::lock_guard<std::mutex> lk(mu_);
	for (const auto& c : repl_) if (c->name == name) return c;
	return nullptr;
}

//---------------------------------------------------------------------------
void Registry::clear()
{
	std::lock_guard<std::mutex> lk(mu_);
	routes_.clear();
	mounts_.clear();
	repl_.clear();
}

} // namespace appserve
