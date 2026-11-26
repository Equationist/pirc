-- Pass 4 negative test: real bound in integer type definition must not ICE in defwalk.a
procedure N48_Real_Int_Range is
   type Bad_Int is range 1.0 .. 10;
begin
   null;
end N48_Real_Int_Range;
