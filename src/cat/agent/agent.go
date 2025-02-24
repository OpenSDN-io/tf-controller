// Package agent provides methods to manage (mock) contrail-vrouter-agent
// configuration objects and processes.
package agent

import (
	"bufio"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io/ioutil"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"cat/sut"

	log "github.com/sirupsen/logrus"
)

const agentName = "control-vrouter-agent"
const agentConfFile = "../agent/contrail-vrouter-agent.conf"
const agentBinary = "../../../../build/debug/vnsw/agent/contrail/contrail-vrouter-agent"
const agentAddPortBinary = "../../../../controller/src/vnsw/agent/port_ipc/vrouter-port-control"

// Agent is a SUT component for VRouter Agent.
type Agent struct {
	sut.Component
	Endpoints []sut.Endpoint
}

// New instatiates a mock contrail-vrouter-agent process.
func New(m sut.Manager, name, binary, test string, endpoints []sut.Endpoint) (*Agent, error) {
	a := &Agent{
		Component: sut.Component{
			Name:    name,
			Manager: m,
			LogDir:  filepath.Join(m.RootDir, test, agentName, name, "log"),
			ConfDir: filepath.Join(m.RootDir, test, agentName, name, "conf"),
			Config: sut.Config{
				BGPPort:  0,
				HTTPPort: 0,
				XMPPPort: 0,
			},
			ConfFile: "",
		},
		Endpoints: endpoints,
	}
	if err := os.MkdirAll(a.Component.ConfDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to make conf directory: %v", err)
	}
	if err := os.MkdirAll(a.Component.LogDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to make log directory: %v", err)
	}
	a.writeConfiguration()
	if err := a.start(binary); err != nil {
		return nil, fmt.Errorf("failed to start agent binary: %v", err)
	}
	if err := a.readAgentHttpPort(); err != nil {
		return nil, fmt.Errorf("failed to read http port for agent: %v", err)
	}
	return a, nil
}

// start starts the mock contrail-vrouter-agent process in the background.
func (a *Agent) start(binary string) error {
	if binary == " " {
		binary = agentBinary
	}
	if _, err := os.Stat(agentBinary); err != nil {
		return fmt.Errorf("failed to get agent binary file: %v", err)
	}
	a.Cmd = exec.Command(agentBinary, "--config_file="+a.Component.ConfFile)
	env := sut.EnvMap{
		"LD_LIBRARY_PATH": "../../../../build/lib",
		"LOGNAME":         os.Getenv("USER"),
	}
	a.Cmd.Env = a.Env(env)
	if err := a.Cmd.Start(); err != nil {
		return fmt.Errorf("Failed to start agent: %v", err)
	}
	return nil
}

// AddVirtualPort adds a mock VMI port into the mocked vrouter agent process.
func (a *Agent) AddVirtualPort(vmi_uuid, vm_uuid, vn_uuid, project_uuid, ipv4_address, mac_address, tap_if, portnum, vm_name string) error {
	uuid := fmt.Sprintf("--uuid=%s", vmi_uuid)
	instance_uuid := fmt.Sprintf("--instance_uuid=%s", vm_uuid)
	vn_uuid_ := fmt.Sprintf("--vn_uuid=%s", vn_uuid)
	vm_project_uuid := fmt.Sprintf("--vm_project_uuid=%s", project_uuid)
	ip_address := fmt.Sprintf("--ip_address=%s", ipv4_address)
	vm_name_ := fmt.Sprintf("--vm_name=%s", vm_name)
	tap_name := fmt.Sprintf("--tap_name=%s", tap_if)
	mac := fmt.Sprintf("--mac=%s", mac_address)
	agent_port := fmt.Sprintf("--agent_port=%s", portnum)

	log.Infof(
		"AddVirtualPort: python3 %s --oper=add %s %s %s %s %s --ipv6_address=  %s %s %s --rx_vlan_id=0 --tx_vlan_id=0 %s",
		agentAddPortBinary, uuid, instance_uuid, vn_uuid_, vm_project_uuid, ip_address, vm_name_, tap_name, mac, agent_port)

	_, err := exec.Command(
		"python3", agentAddPortBinary, "--oper=add", uuid, instance_uuid, vn_uuid_, vm_project_uuid, ip_address,
		"--ipv6_address=", vm_name_, tap_name, mac, "--rx_vlan_id=0", "--tx_vlan_id=0", agent_port).Output()

	return err
}

// writeConfiguration generates agent configuration with appropriate xmpp
// server port numbers into agentConfFile.
func (a *Agent) writeConfiguration() error {
	file, err := os.Open(agentConfFile)
	if err != nil {
		return fmt.Errorf("failed to open agent conf file: %v", err)
	}
	defer file.Close()
	var config []string
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := scanner.Text()
		var confLine string
		switch {
		case strings.Contains(line, "xmpp_port"):
			confLine = "servers="
			for _, endpoint := range a.Endpoints {
				confLine = fmt.Sprintf("%s%s:%d ", confLine, endpoint.IP, endpoint.Port)
			}
		case strings.Contains(line, "agent_name"):
			confLine = fmt.Sprintf("agent_name=%s", a.Name)
		case strings.Contains(line, "log_file"):
			confLine = fmt.Sprintf("log_file=%s/%s.log", a.LogDir, a.Name)
		default:
			confLine = line
		}
		config = append(config, confLine)
	}

	a.Component.ConfFile = fmt.Sprintf("%s/%s.conf", a.Component.ConfDir, a.Component.Name)
	if err := ioutil.WriteFile(a.Component.ConfFile, []byte(strings.Join(config, "\n")), 0644); err != nil {
		return fmt.Errorf("failed to write to agent conf file: %v", err)
	}
	return nil
}

func (a *Agent) readAgentHttpPort() error {
	a.PortsFile = fmt.Sprintf("%d.json", a.Cmd.Process.Pid)
	retry := 30
	var err error
	for retry > 0 {
		if bytes, err := ioutil.ReadFile(a.PortsFile); err == nil {
			if err := json.Unmarshal(bytes, &a.Config); err == nil {
				return nil
			}
		}
		time.Sleep(1 * time.Second)
		retry = retry - 1
	}
	return err
}

type ItfRespList struct {
	XMLName xml.Name `xml:"__ItfResp_list"`
	Text    string   `xml:",chardata"`
	Type    string   `xml:"type,attr"`
	ItfResp struct {
		Text    string `xml:",chardata"`
		Type    string `xml:"type,attr"`
		ItfList struct {
			Text       string `xml:",chardata"`
			Type       string `xml:"type,attr"`
			Identifier string `xml:"identifier,attr"`
			List       struct {
				Text           string `xml:",chardata"`
				Type           string `xml:"type,attr"`
				Size           string `xml:"size,attr"`
				ItfSandeshData struct {
					Text  string `xml:",chardata"`
					Index struct {
						Text       string `xml:",chardata"`
						Type       string `xml:"type,attr"`
						Identifier string `xml:"identifier,attr"`
					} `xml:"index"`
					Name struct {
						Text       string `xml:",chardata"`
						Type       string `xml:"type,attr"`
						Identifier string `xml:"identifier,attr"`
					} `xml:"name"`
					Uuid struct {
						Text       string `xml:",chardata"`
						Type       string `xml:"type,attr"`
						Identifier string `xml:"identifier,attr"`
					} `xml:"uuid"`
					VrfName struct {
						Text       string `xml:",chardata"`
						Type       string `xml:"type,attr"`
						Identifier string `xml:"identifier,attr"`
						Link       string `xml:"link,attr"`
					} `xml:"vrf_name"`
					Active struct {
						Text       string `xml:",chardata"`
						Type       string `xml:"type,attr"`
						Identifier string `xml:"identifier,attr"`
					} `xml:"active"`
				} `xml:"ItfSandeshData"`
			} `xml:"list"`
		} `xml:"itf_list"`
		More struct {
			Text       string `xml:",chardata"`
			Type       string `xml:"type,attr"`
			Identifier string `xml:"identifier,attr"`
		} `xml:"more"`
	} `xml:"ItfResp"`
}

// Check interface Active/Incative state in agent introspect
func (a *Agent) VerifyIntrospectInterfaceState(intf string, active bool) error {
	url := fmt.Sprintf("http://%s:%d/Snh_ItfReq?name=%s", "0.0.0.0", a.Config.HTTPPort, intf)
	resp, err := http.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	log.Infof("Get request %s completed with status %v\n", url, err)

	// Parse response
	var parse_resp ItfRespList
	data, err := ioutil.ReadAll(resp.Body)
	xml.Unmarshal(data, &parse_resp)

	if parse_resp.ItfResp.ItfList.List.ItfSandeshData.Active.Text != "Active" && active {
		return fmt.Errorf("interface not active in agent: %v ", err)
	}

	return nil
}
