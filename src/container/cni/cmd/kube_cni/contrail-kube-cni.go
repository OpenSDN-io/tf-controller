// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
//
// Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
//
/****************************************************************************
 * Main routines for kubernetes CNI plugin
 ****************************************************************************/
package main

import (
	"opensdn-k8s-cni/internal/contrail"
	log "opensdn-k8s-cni/internal/logging"

	"github.com/containernetworking/cni/pkg/skel"
	cniSpecVersion "github.com/containernetworking/cni/pkg/version"
)

// Add command
func CmdAdd(skelArgs *skel.CmdArgs) error {
	// Initialize Contrail module
	cni, err := contrail.Init(skelArgs)
	if err != nil {
		return err
	}

	log.Infof("Came in Add for container %s", skelArgs.ContainerID)

	// Handle Add command
	err = cni.CmdAdd()
	if err != nil {
		log.Errorf("Failed processing Add command.")
		return err
	}

	return nil
}

// Del command
func CmdDel(skelArgs *skel.CmdArgs) error {
	// Initialize Contrail module
	cni, err := contrail.Init(skelArgs)
	if err != nil {
		return err
	}

	log.Infof("Came in Del for container %s", skelArgs.ContainerID)

	// Handle Del command
	err = cni.CmdDel()
	if err != nil {
		log.Errorf("Failed processing Del command.")
		return err
	}

	return nil
}

func main() {
	// Let CNI skeletal code handle demux based on env variables
	funcs := skel.CNIFuncs{
		Add: CmdAdd,
		Del: CmdDel,
	}
	skel.PluginMainFuncs(funcs, cniSpecVersion.All, "")
}
