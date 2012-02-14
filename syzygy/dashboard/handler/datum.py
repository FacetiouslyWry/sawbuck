# Copyright 2011 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import json
import urlparse
from google.appengine.ext import webapp
from model import client as client_db
from model import datum as datum_db
from model import metric as metric_db
from model import product as product_db


class DatumHandler(webapp.RequestHandler):
  """A class to handle creating and querying data.

  Handles GET, POST, PUT AND DELETE requests for
  /data/<product>/<metric>/<client> and
  /data/<product>/<metric>/<client>/<datum>. All functions have the same
  signature, even though they may not use all parameters, so that a single route
  can be used for the handler.
  """

  def get(self, product_id, client_id, metric_id, datum_id):
    """Responds with information about all data or a specific datum.

    /data/<product>/<client>/<metric>/
      Responds with a JSON encoded object that contains a list of data for the
      given product, client and metric.
    /data/<product>/<client>/<metric>/<datum>
      Responds with a JSON encoded object or the product ID, client ID, metric
      ID, product version, toolchain version, timestamp and values for the given
      product, client, metric and datum.

    Args:
      product_id: The product ID.
      client_id: The client ID.
      metric_id: The metric ID.
      datum_id: The datum ID. May be empty.
    """
    product = product_db.Product.get_by_key_name(product_id)
    if not product:
      self.error(404)  # Not found.
      return

    client = client_db.Client.get_by_key_name(client_id, product)
    if not client:
      self.error(404)  # Not found.
      return

    metric = metric_db.Metric.get_by_key_name(metric_id, client)
    if not metric:
      self.error(404)  # Not found.
      return

    result = {'product_id': product.key().name(),
              'client_id': client.key().name(),
              'metric_id': metric.key().name()}

    if not datum_id:
      data = datum_db.Datum.all()
      data.ancestor(metric)

      data_result = []
      for datum in data:
        data_result.append({'datum_id': datum.key().id(),
                            'product_version': datum.product_version,
                            'toolchain_version': datum.toolchain_version,
                            'timestamp': str(datum.timestamp),
                            'values': datum.values})
      result.update({'data': data_result})
    else:
      try:
        datum_id = int(datum_id)
      except ValueError:
        self.error(400)  # Bad request.
        return

      datum = datum_db.Datum.get_by_id(datum_id, metric)
      if not datum:
        self.error(404)  # Not found.
        return

      result.update({'datum_id': datum.key().id(),
                     'product_version': datum.product_version,
                     'toolchain_version': datum.toolchain_version,
                     'timestamp': str(datum.timestamp),
                     'values': datum.values})

    self.response.headers['Content-Type'] = 'application/json'
    json.dump(result, self.response.out)

  def post(self, product_id, client_id, metric_id, datum_id):
    """Creates a new datum.

    /data/<product>/<client>/<metric>
      Creates a new datum. The product version, toolchain version and values
      should be specified in the body of the request. Responds with a JSON
      encoded datum ID.
    /data/<product>/<client>/<metric>/<datum>
      Unused.

    Args:
      product_id: The product ID.
      client_id: The client ID.
      metric_id: The metric ID.
      datum_id: The datum ID. Must be empty.
    """
    # Validate input.
    if datum_id:
      self.error(400)  # Bad request.
      return

    product_version = self.request.get('product_version', None)
    toolchain_version = self.request.get('toolchain_version', None)
    values = self.request.get_all('values')
    if not product_version or not toolchain_version or not values:
      self.error(400)  # Bad request.
      return

    try:
      values = [float(value) for value in values]
    except ValueError:
      self.error(400)  # Bad request.
      return

    # Perform DB lookups.
    product = product_db.Product.get_by_key_name(product_id)
    if not product:
      self.error(404)  # Not found.
      return

    client = client_db.Client.get_by_key_name(client_id, product)
    if not client:
      self.error(404)  # Not found.
      return

    metric = metric_db.Metric.get_by_key_name(metric_id, client)
    if not metric:
      self.error(404)  # Not found.
      return

    # Create a new datum.
    datum = datum_db.Datum(parent=metric, product_version=product_version,
                           toolchain_version=toolchain_version, values=values)
    datum.put()

    result = {'datum_id': datum.key().id()}

    self.response.headers['Content-Type'] = 'application/json'
    json.dump(result, self.response.out)

  def put(self, product_id, client_id, metric_id, datum_id):
    """Updates a datum.

    /data/<product>/<client>/<metric>
      Unused.
    /data/<product>/<client>/<metric>/<datum>
      Updates a datum. The product version, toolchain version and values should
      be specified in the body of the request.

    Args:
      product_id: The product ID.
      client_id: The client ID.
      metric_id: The metric ID.
      datum_id: The datum ID. Must not be empty.
    """
    # Validate input
    if not datum_id:
      self.error(400)  # Bad request.
      return

    try:
      datum_id = int(datum_id)
    except ValueError:
      self.error(400)  # Bad request.
      return

    # Appengine bug: parameters in body aren't parsed for PUT requests.
    # http://code.google.com/p/googleappengine/issues/detail?id=170
    params = urlparse.parse_qs(self.request.body)
    product_version = params.get('product_version', [None])[0]
    toolchain_version = params.get('toolchain_version', [None])[0]
    values = params.get('values', [])
    if not product_version or not toolchain_version or not values:
      self.error(400)  # Bad request.
      return

    try:
      values = [float(value) for value in values]
    except ValueError:
      self.error(400)  # Bad request.
      return

    # Perform DB lookups.
    product = product_db.Product.get_by_key_name(product_id)
    if not product:
      self.error(404)  # Not found.
      return

    client = client_db.Client.get_by_key_name(client_id, product)
    if not client:
      self.error(404)  # Not found.
      return

    metric = metric_db.Metric.get_by_key_name(metric_id, client)
    if not metric:
      self.error(404)  # Not found.
      return

    datum = datum_db.Datum.get_by_id(datum_id, metric)
    if not datum:
      self.error(404)  # Not found.
      return

    # Update the datum.
    datum.product_version = product_version
    datum.toolchain_version = toolchain_version
    datum.values = values
    datum.put()

  def delete(self, product_id, client_id, metric_id, datum_id):
    """Deletes a datum.

    /data/<product>/<client>/<metric>
      Unused.
    /data/<product>/<client>/<metric>/<datum>
      Deletes the specified datum.

    Args:
      product_id: The product ID.
      client_id: The client ID.
      metric_id: The metric ID.
      datum_id: The datum ID. Must not be empty.
    """
    # Validate input
    if not datum_id:
      self.error(400)  # Bad request.
      return

    try:
      datum_id = int(datum_id)
    except ValueError:
      self.error(400)  # Bad request.
      return

    # Perform DB lookups.
    product = product_db.Product.get_by_key_name(product_id)
    if not product:
      self.error(404)  # Not found.
      return

    client = client_db.Client.get_by_key_name(client_id, product)
    if not client:
      self.error(404)  # Not found.
      return

    metric = metric_db.Metric.get_by_key_name(metric_id, client)
    if not metric:
      self.error(404)  # Not found.
      return

    # Delete the metric.
    datum = datum_db.Datum.get_by_id(datum_id, metric)
    if not datum:
      self.error(400)  # Bad request.
      return

    datum.delete()