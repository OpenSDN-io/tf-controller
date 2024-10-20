from __future__ import unicode_literals
#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

from vnc_api.utils import (OP_POST, OP_GET, OP_PUT, OP_DELETE, hdr_client_tenant)

# TODO transform from client value


def hdr_server_tenant():
    return 'HTTP_X_TENANT_NAME'
# end hdr_tenant_server


class LinkObject(object):

    def __init__(self, rel, base_url, uri, name, http_method=None):
        self.rel = rel
        self.base_url = base_url
        self.uri = uri
        self.name = name
        self.http_method = http_method
    # end __init__

    def to_dict(self, with_url=None):
        if not with_url:
            url = self.base_url
        else:
            url = with_url
        return {'rel': self.rel,
                'href': url + self.uri,
                'name': self.name,
                'method': self.http_method}
    # end to_dict

# end class LinkObject
