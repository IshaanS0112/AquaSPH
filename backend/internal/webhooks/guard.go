package webhooks

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"syscall"
	"time"
)

// A tenant chooses the URL, so the dispatcher is a request forger aimed wherever they like:
// cloud metadata at 169.254.169.254, Postgres on localhost, the internal metrics port. The
// guard checks every connection's resolved address at dial time, which also covers DNS
// rebinding (a name that resolves publicly when registered and privately when delivered), and
// redirects are not followed.

var ErrBlockedAddress = errors.New("webhook target resolves to a private or reserved address")

var blockedPrefixes = []netip.Prefix{
	netip.MustParsePrefix("0.0.0.0/8"),
	netip.MustParsePrefix("100.64.0.0/10"), // carrier-grade NAT
	netip.MustParsePrefix("192.0.0.0/24"),
	netip.MustParsePrefix("198.18.0.0/15"), // benchmarking
	netip.MustParsePrefix("240.0.0.0/4"),
	netip.MustParsePrefix("64:ff9b::/96"), // NAT64 can reach IPv4 private space
}

// publicAddress reports whether ip is an address a tenant may make the dispatcher connect to.
func publicAddress(ip netip.Addr) bool {
	ip = ip.Unmap()
	if !ip.IsValid() || ip.IsLoopback() || ip.IsPrivate() || ip.IsLinkLocalUnicast() ||
		ip.IsLinkLocalMulticast() || ip.IsInterfaceLocalMulticast() || ip.IsMulticast() || ip.IsUnspecified() {
		return false
	}
	for _, p := range blockedPrefixes {
		if p.Contains(ip) {
			return false
		}
	}
	return true
}

// ValidateURL checks an endpoint URL at registration. allowInsecure (development) permits plain
// http and private targets; production requires https and a public host.
func ValidateURL(raw string, allowInsecure bool) error {
	if len(raw) > 2048 {
		return errors.New("url is longer than 2048 characters")
	}
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" || u.Opaque != "" {
		return errors.New("url must be an absolute http(s) URL")
	}
	switch {
	case u.Scheme == "https":
	case u.Scheme == "http" && allowInsecure:
	default:
		return errors.New("url must use https")
	}
	if u.User != nil {
		return errors.New("url must not contain credentials")
	}
	if u.Fragment != "" {
		return errors.New("url must not contain a fragment")
	}
	if allowInsecure {
		return nil
	}
	host := u.Hostname()
	if host == "localhost" || len(host) > 10 && host[len(host)-10:] == ".localhost" {
		return ErrBlockedAddress
	}
	if ip, err := netip.ParseAddr(host); err == nil && !publicAddress(ip) {
		return ErrBlockedAddress
	}
	return nil
}

// NewHTTPClient returns the dispatcher's client. It ignores proxy environment variables: behind
// a proxy the dial-time check would see the proxy's address, not the target's.
func NewHTTPClient(timeout time.Duration, allowPrivate bool) *http.Client {
	dialer := &net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}
	if !allowPrivate {
		dialer.Control = func(network, address string, _ syscall.RawConn) error {
			ap, err := netip.ParseAddrPort(address)
			if err != nil {
				return fmt.Errorf("%w: %s", ErrBlockedAddress, address)
			}
			if !publicAddress(ap.Addr()) {
				return fmt.Errorf("%w: %s", ErrBlockedAddress, ap.Addr())
			}
			return nil
		}
	}
	transport := &http.Transport{
		Proxy:                 nil,
		DialContext:           func(ctx context.Context, n, a string) (net.Conn, error) { return dialer.DialContext(ctx, n, a) },
		ForceAttemptHTTP2:     true,
		MaxIdleConns:          64,
		MaxIdleConnsPerHost:   4,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   5 * time.Second,
		ResponseHeaderTimeout: timeout,
	}
	return &http.Client{
		Transport: transport,
		Timeout:   timeout,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
}
