require 'minitest/autorun'
require 'tmpdir'
require_relative 'site'

class SiteTest < Minitest::Test
  def test_publication_address
    assert_equal({ 'url' => '', 'baseurl' => '' }, DocumentationSite.location(''))
    assert_equal('/epd/docs', DocumentationSite.location('https://docs.example.com/epd/docs/')['baseurl'])
    ['http://example.com/docs/', 'https://name:secret@example.com/docs', 'https://example.com/docs?q=1',
     'https://example.com/docs#part', 'https://example.com/a//b', 'https://example.com/a%2fb'].each do |url|
      assert_raises(RuntimeError) { DocumentationSite.location(url) }
    end
  end

  def test_collection_and_component_sources
    Dir.mktmpdir('epd-docs-test') do |destination|
      workflow = YAML.load_file(File.join(DocumentationSite::ROOT, '../.github/workflows/deploy-docs.yml'))
      target = DocumentationSite.location(workflow.fetch('env').fetch('DOCS_SITE_URL'))
      publisher = workflow.fetch('jobs').fetch('publish').fetch('steps').find { |step| step['uses'] == 'OpenSiFli/SiFliMirrorSync@v1' }
      assert_equal target.fetch('baseurl') + '/', publisher.fetch('with').fetch('prefix')
      base = target.fetch('baseurl')
      site = Jekyll::Site.new(DocumentationSite.configuration(target.merge('destination' => destination)))
      site.process
      DocumentationSite.verify(destination)
      assert_equal 46, site.data['article_count']
      components = site.pages.select { |page| page.data['doc_id']&.start_with?('components/') }
      assert_equal 5, components.length
      (site.collections['docs'].docs + components).each do |page|
        rendered = Nokogiri::HTML(page.output)
        assert_equal page.data['toc'].length, rendered.css('.page-toc a').length, page.data['doc_id']
        group = site.data['groups'].find { |entry| entry['id'] == page.data['group'] }
        position = group['pages'].index { |entry| entry['id'] == page.data['doc_id'] }
        neighbors = []
        neighbors << group['pages'][position - 1] if position.positive?
        neighbors << group['pages'][position + 1] if position + 1 < group['pages'].length
        assert_equal neighbors.map { |entry| base + entry['url'] },
                     rendered.css('.page-neighbors a').map { |link| link['href'] }, page.data['doc_id']
        source = File.read(page.data['source_file'], encoding: 'UTF-8')
        source.scan(/^```[^\n]*\n(.*?)^```/m).each do |match|
          code = match.first.gsub("\r\n", "\n").strip
          assert_includes rendered.css('pre').map(&:text).join("\n"), code
        end
      end
      font = components.find { |page| page.data['doc_id'] == 'components/font/README.md' }
      assert_includes font.output, base + '/assets/site.css'
      assert_includes font.output, '查看源码'
      assert_includes File.read(File.join(destination, 'search-index.json')), '字形'
      assert_includes File.read(File.join(destination, 'components/index.html')), base + '/components/font/'
      refute Dir.exist?(File.join(DocumentationSite::ROOT, 'source/_docs/components'))
    end
  end
end
