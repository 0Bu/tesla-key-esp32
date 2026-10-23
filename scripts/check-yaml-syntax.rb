#!/usr/bin/env ruby
# Real offline YAML syntax gate using Ruby's standard Psych parser. The separate Python workflow
# policy checker owns GitHub-specific DAG, permission, pin, and trust-boundary semantics.
require "tmpdir"
require "yaml"
require "json"

def validate(paths)
  raise "no YAML files supplied" if paths.empty?
  paths.each do |path|
    document = YAML.parse_file(path)
    raise "#{path}: YAML document is empty" if document.nil?
  rescue Psych::SyntaxError => e
    raise "#{path}: invalid YAML: #{e.message}"
  end
end

# Preserve scalar spelling (especially GitHub's `on` key) instead of YAML 1.1 boolean coercion.
# The policy model accepts block and flow collections, refuses aliases and duplicate keys.
def policy_model(node)
  case node
  when Psych::Nodes::Scalar
    node.value
  when Psych::Nodes::Sequence
    node.children.map { |child| policy_model(child) }
  when Psych::Nodes::Mapping
    result = {}
    node.children.each_slice(2) do |key, value|
      raise "policy YAML keys must be scalars" unless key.is_a?(Psych::Nodes::Scalar)
      raise "duplicate policy YAML key: #{key.value}" if result.key?(key.value)
      result[key.value] = policy_model(value)
    end
    result
  else
    raise "policy YAML aliases or unsupported nodes are forbidden"
  end
end

if ARGV.delete("--json")
  begin
    raise "--json requires exactly one YAML file" unless ARGV.length == 1
    document = YAML.parse_file(ARGV[0])
    raise "empty policy YAML" if document.nil?
    puts JSON.generate(policy_model(document.root))
  rescue RuntimeError, Psych::SyntaxError => e
    warn "yaml-model: #{e.message}"
    exit 1
  end
  exit 0
end

self_test = ARGV.delete("--self-test")
begin
  validate(ARGV)
  if self_test
    Dir.mktmpdir("yaml-syntax-selftest-") do |directory|
      malformed = File.join(directory, "malformed.yml")
      File.write(malformed, "jobs:\n  broken: [\n")
      begin
        validate([malformed])
      rescue RuntimeError => e
        raise unless e.message.include?("invalid YAML")
      else
        raise "malformed YAML mutation was accepted"
      end
    end
  end
rescue RuntimeError => e
  warn "yaml-syntax: #{e.message}"
  exit 1
end

puts "yaml-syntax: PASS (#{ARGV.length} files#{self_test ? ", malformed mutation canary" : ""})"
