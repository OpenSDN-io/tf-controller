module opensdn-k8s-cni

go 1.23

toolchain go1.23.4

require (
	github.com/containernetworking/cni v1.2.3
	github.com/containernetworking/plugins v1.6.2
	github.com/natefinch/lumberjack v2.0.0+incompatible
	github.com/vishvananda/netlink v1.3.0
)

require (
	github.com/BurntSushi/toml v1.4.0 // indirect
	github.com/coreos/go-iptables v0.8.0 // indirect
	github.com/safchain/ethtool v0.5.9 // indirect
	github.com/vishvananda/netns v0.0.4 // indirect
	golang.org/x/sys v0.27.0 // indirect
	gopkg.in/natefinch/lumberjack.v2 v2.2.1 // indirect
	gopkg.in/yaml.v2 v2.4.0 // indirect
	sigs.k8s.io/knftables v0.0.18 // indirect
)
