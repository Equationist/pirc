-- Pass 4 negative test: selected name in context USE clause must not ICE in libphase/semphase
with TEXT_IO;
use TEXT_IO.INTEGER_IO;
procedure N50_Context_Sel_Use is
begin
   null;
end N50_Context_Sel_Use;
