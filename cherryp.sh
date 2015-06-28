git rev-list --reverse --topo-order d17b3a329053b2a5dad7e8fdbd7760969b490b94^..b23c14a7f9a642881801fac124dc87462fd17ccf | while read rev 
do 
  git cherry-pick $rev || break 
done 
