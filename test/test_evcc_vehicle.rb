#!/usr/bin/env ruby
# Exercise the shipped YAML and its actual jq expression, without a live endpoint.
require 'json'
require 'open3'
require 'yaml'

ROOT = File.expand_path('..', __dir__)
SOURCE = File.read(File.join(ROOT, 'docs/examples/evcc-custom-vehicle.yaml'))

def require_contract(condition, message)
  raise message unless condition
end

def check_example(config)
  %w[chargeEnable maxcurrent wakeup].each do |name|
    plugin = config.fetch(name)
    require_contract(plugin.fetch('timeout') == '30s', "#{name} needs an explicit 30s deadline")
    require_contract(plugin.fetch('method') == 'POST', "#{name} must remain POST")
  end
  require_contract(config.fetch('maxcurrent').fetch('body') == '{"charging_amps": ${maxcurrent}}', 'current payload changed')
  require_contract(config.fetch('chargeEnable').fetch('uri').end_with?('/command/{{ if .chargeenable }}charge_start{{ else }}charge_stop{{ end }}'), 'charge-enable command selection changed')
  require_contract(config.fetch('wakeup').fetch('uri').end_with?('/command/wake_up'), 'wake command changed')
  states = {'Starting' => 'B', 'Charging' => 'C', 'Stopped' => 'B', 'NoPower' => 'B',
            'Complete' => 'B', 'Disconnected' => 'A', 'Unknown' => 'A'}
  states.each do |state, expected|
    input = JSON.generate({'response' => {'response' => {'charge_state' => {'charging_state' => state}}}})
    output, error, status = Open3.capture3('jq', '-er', config.fetch('status').fetch('jq'), stdin_data: input)
    require_contract(status.success? && output.strip == expected, "#{state} must map to #{expected}: #{error}")
  end
end

config = YAML.safe_load(SOURCE, permitted_classes: [], aliases: false)
check_example(config)

# The historical mapping and an omitted setter timeout must both fail this same check.
old = YAML.safe_load(SOURCE.lines.reject { |line| line.include?('== "Starting"') }.join,
                     permitted_classes: [], aliases: false)
missing_timeout = Marshal.load(Marshal.dump(config))
missing_timeout.fetch('maxcurrent').delete('timeout')
[old, missing_timeout].each do |negative|
  rejected = false
  begin
    check_example(negative)
  rescue RuntimeError, KeyError
    rejected = true
  end
  require_contract(rejected, 'broken evcc example passed its regression gate')
end
puts 'evcc YAML: actual jq state table, setter deadlines/bodies and two negative controls passed'
