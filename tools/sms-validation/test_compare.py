import unittest
from compare import compare

class ComparisonTest(unittest.TestCase):
    def sample(self, rss):
        return {'status':'passed','scenario':'ready','memory_mib':48,'gomaxprocs':1,
                'gogc':20,'soft_go_memory_limit_mib':12,'poll_seconds':3,
                'kernel_sha256':'kernel','fixture_sha256':'fixture','fixture_source_sha256':{'init.sh':'init'},'binary_sha256':str(rss),
                'samples':{p:{'rss_kib':rss,'pss_kib':rss,'anonymous_kib':rss-1000}
                           for p in ('startup','poll','http','pressure','released')}}
    def test_reports_absolute_delta_with_fixed_environment(self):
        a,b=self.sample(7000),self.sample(6200)
        result=compare(a,b)
        self.assertEqual(result.get('phases',{}).get('http',{}).get('rss_delta_kib'),-800)
        for key,value in [('fixture_sha256','different'),('fixture_source_sha256',{'init.sh':'other'}),('kernel_sha256','other'),('gogc',10),('status','failed')]:
            broken=dict(b);broken[key]=value
            with self.assertRaises(ValueError): compare(a,broken)

if __name__=='__main__':unittest.main()
