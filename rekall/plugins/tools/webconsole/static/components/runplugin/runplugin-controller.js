'use strict';
(function() {
  var module = angular.module('rekall.runplugin.controller',
                              ['manuskript.core',
                               'manuskript.core.network.service',
                               'rekall.runplugin.contextMenu.directive',
                               'rekall.runplugin.freeFormat.directive',
                               'rekall.runplugin.jsonDecoder.service',
                               'rekall.runplugin.objectActions.init',
                               'rekall.runplugin.objectActions.service',
                               'rekall.runplugin.objectRenderer.service',
                               'rekall.runplugin.pluginArguments.directive',
                               'rekall.runplugin.pluginRegistry.service',
                               'rekall.runplugin.rekallTable.directive',
                               'pasvaz.bindonce']);

  module.controller('RekallRunPluginController', function(
    $scope, $filter, manuskriptNetworkService,
    rekallPluginRegistryService, rekallJsonDecoderService) {

    $scope.search = {
      pluginName: ''
    };

    $scope.plugins = [];

    rekallPluginRegistryService.getPlugins(function(result) {
      $scope.plugins = result;

      $scope.pluginsValues = [];
      for (var key in $scope.plugins) {
        $scope.pluginsValues.push($scope.plugins[key]);
      }
    });

    // If the plugin changes, we need to modify the arguments.
    $scope.$watch('node.source.plugin', function() {
      if ($scope.node.source.plugin) {
        $scope.requiredArguments = $filter('filter')($scope.node.source.plugin.arguments, {
          required: true
        });
        $scope.optionalArguments = $filter('filter')($scope.node.source.plugin.arguments, {
          required: false
        });
      }
    });


    // When data appears in the plugin_state parameter we want to copy _some_ of
    // it into node.rendered for rendering.
    var copyStateToRendered = function() {
      if (!$scope.node.plugin_state) {
        return;
      };

      // For now just copy everything.
      $scope.node.rendered = angular.copy($scope.node.plugin_state.elements);
      return

      var last_visible_element = $scope.view_port_min + $scope.node.rendered.length;

      // There is more data available.
      if (last_visible_element < $scope.node.plugin_state.elements.length) {

        // Concatenate the required number of elements to the end of the
        // rendered array.
        $scope.node.rendered = $scope.node.rendered.concat(
          $scope.node.plugin_state.elements.slice(
            last_visible_element, $scope.view_port_max));
      }
    };

    $scope.pushSources = function() {
      if ($scope.node.source.plugin) {
        var state = rekallJsonDecoderService.createEmptyState();
        var queue = [];

        // We hold the plugin state here.
        $scope.node.plugin_state = state;
        $scope.node.state = 'render';
        manuskriptNetworkService.callServer('rekall/runplugin', {
          params: $scope.node.source,
          onclose: function(msg) {  // jshint ignore:line
            $scope.node.state = 'show';
          },
          onmessage: function(jsonOutput) {
            for (var i = 0; i < jsonOutput.length; ++i) {
              queue.push(jsonOutput[i]);
            }
            $scope.$evalAsync(function() {
              if (queue.length > 0) {
                rekallJsonDecoderService.decode(queue, state);

                $scope.node.source.cookie = state.metadata.cookie;
                copyStateToRendered();

                queue.splice(0, queue.length);
              }
            });
          }});

      } else {
        $scope.node.plugin_state = {
          stderr: ['No Rekall plugin was selected.'],
          stdout: [],
          error: []
        };
        $scope.node.state = 'show';
      }
    };

    // Total number of elements in the view port.
    $scope.view_port_min = 0;
    $scope.view_port_max = 10;

    if ($scope.node.rendered == null) {
      $scope.node.rendered = [];
    };

    $scope.$watch('node.state', function() {
      if ($scope.node.state === 'render') {
        $scope.pushSources();
      } else if ($scope.node.state === "edit") {
        $scope.node.rendered = [];

        // Force re-calculation on the server side as cache is likely stale.
        $scope.node.source.cookie = null;
      }
    });

    $scope.minimized = true;
    $scope.minimizeToggle = function($event) {
      var button = $($event.target);
      button.toggleClass("minimized");
      if ($scope.minimized) {
        $scope.height = 500;
      } else {
        $scope.height = 120;
      };
      $scope.minimized = !$scope.minimized;

      $event.stopPropagation();
    };

  });

})();